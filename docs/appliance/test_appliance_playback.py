"""Exercise appliance playback against a running Kodi fork with disposable userdata."""

import argparse
import json
import socket
import sqlite3
import subprocess
import tempfile
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Event, Thread
from time import monotonic, sleep
from urllib.parse import parse_qs, urlsplit

from test_stop_signal import KodiConnection


def write_json(path, data):
    path.write_text(json.dumps(data), encoding="utf-8")


class Provider(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body, content_type, extra_headers=()):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for name, value in extra_headers:
            self.send_header(name, value)
        self.end_headers()
        if self.command == "GET":
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

    def do_GET(self):
        url = urlsplit(self.path)
        query = parse_qs(url.query)
        if url.path == "/api/v1/search":
            self.server.calls["discover"] += 1
            self.server.discovered_at = monotonic()
            self.server.discovery_started.set()
            assert self.server.release_discovery.wait(5), "Timed out waiting for the test client"
            assert query["query"] == ["Pilot Show"]
            assert query["limit"] == ["50"]
            assert "X-Api-Key" not in self.headers
            releases = [
                {"infoHash": digit * 40, "title": "Pilot.Show.S01.1080p.WEB-DL",
                 "size": pack_bytes}
                for digit, pack_bytes in (("a", 50_000_000_000), ("b", 45_000_000_000),
                                          ("C", 40_000_000_000))
            ]
            self.reply(200, json.dumps(releases).encode(), "application/json")
            self.server.discovery_replied.set()
        elif url.path == "/v1/api/torrents/mylist":
            self.server.calls["list"] += 1
            assert query["limit"] == ["200"]
            assert "Authorization" not in self.headers
            confirmed = {"id": 4, "name": "Pilot.Show.S01E02.Return.1080p.WEB-DL.mkv",
                         "size": len(self.server.media)}
            pack = [
                {"id": 2, "name": "Pilot.Show.S01E02.Wrong.mkv", "size": 1_000_000_000},
                {"id": 3, "name": "Pilot.Show.S01E03.Return.mkv", "size": 1_100_000_000},
                confirmed,
            ]
            ambiguous = [dict(confirmed, size=1_800_000_000),
                         {"id": 5, "name": "other/" + confirmed["name"]}]
            torrents = [
                {"id": torrent_id, "hash": digit * 40, "download_finished": True,
                 "download_present": True, "files": files}
                for torrent_id, digit, files in (
                    (20, "a", [dict(confirmed, size=1024)]), (30, "b", ambiguous),
                    (12, "c", [dict(confirmed, id=7)]), (11, "C", pack))
            ]
            self.reply(200, json.dumps({"success": True, "data": torrents}).encode(),
                       "application/json")
            self.server.listed_at = monotonic()
        elif url.path == "/v1/api/torrents/requestdl":
            started = monotonic()
            self.server.calls["resolve"] += 1
            assert "token" not in query
            route = (int(query["torrent_id"][0]), int(query["file_id"][0]))
            self.server.resolved_routes.append(route)
            assert route in {(11, 4), (12, 7), (20, 4)}, route
            expires = route == (11, 4) and (
                self.server.mode == "alternate" or
                (self.server.mode == "refresh" and self.server.calls["resolve"] == 1))
            source = "expired.mkv" if expires else "playing.mkv"
            if self.server.mode == "break" and route[0] != 20:
                source = "breaking.mp4"
            if self.server.mode == "next-file" and route[0] != 20:
                reply = {"success": False}
            else:
                reply = {"success": True,
                         "data": f"http://127.0.0.1:{self.server.server_port}/{source}"}
            self.reply(200, json.dumps(reply).encode(), "application/json")
            if hasattr(self.server, "resolve_seconds"):
                self.server.resolve_seconds.append((route, monotonic() - started))
        elif url.path == "/expired.mkv":
            self.server.calls["expired"] += 1
            self.reply(403, b"expired", "text/plain")
        elif url.path in ("/playing.mkv", "/breaking.mp4"):
            self.server.calls["playing"] += 1
            body = self.server.media
            byte_range = self.headers.get("Range", "")
            start, end = 0, len(body) - 1
            headers = [("Accept-Ranges", "bytes")]
            if byte_range.startswith("bytes="):
                from_byte, _, through_byte = byte_range[6:].partition("-")
                start = int(from_byte)
                if start >= len(body):
                    self.reply(416, b"", "video/x-matroska")
                    return
                end = min(int(through_byte) if through_byte else len(body) - 1, len(body) - 1)
                extent = f"bytes {start}-{end}/{len(body)}"
                headers.append(("Content-Range", extent))
            status = 206 if byte_range.startswith("bytes=") else 200
            if url.path == "/breaking.mp4":
                self.send_response(status)
                self.send_header("Content-Type", "video/mp4")
                self.send_header("Content-Length", str(end - start + 1))
                for name, value in headers:
                    self.send_header(name, value)
                self.end_headers()
                if self.command == "GET":
                    try:
                        self.wfile.write(body[start : min(end + 1, len(body) * 3 // 5)])
                        self.server.break_read.wait(20)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
            else:
                self.reply(status, body[start : end + 1], "video/x-matroska", headers)
        else:
            self.reply(404, b"", "text/plain")

    do_HEAD = do_GET


class Kodi(KodiConnection):
    def messages(self, timeout):
        deadline = monotonic() + timeout
        while True:
            self.buffer = self.buffer.lstrip()
            try:
                message, end = self.json_decoder.raw_decode(self.buffer)
            except json.JSONDecodeError:
                remaining = deadline - monotonic()
                if remaining <= 0:
                    raise TimeoutError("Kodi stopped sending JSON-RPC messages")
                self.connection.settimeout(remaining)
                packet = self.connection.recv(4096)
                if not packet:
                    raise ConnectionError("Kodi disconnected")
                self.buffer += self.utf8_decoder.decode(packet)
                continue
            self.buffer = self.buffer[end:]
            yield message

    def call(self, method, **params):
        request_id = self.request(method, **params)
        for message in self.messages(10):
            if "error" in message:
                raise RuntimeError(f"{method}: {message['error']}")
            if message.get("id") == request_id:
                return message["result"]

    def wait_for(self, method, timeout=10):
        for message in self.messages(timeout):
            if "error" in message:
                raise RuntimeError(message["error"])
            if message.get("method") == method:
                return message["params"]["data"]

    def play(self, path, resume=False, timeout=10):
        self.request("Player.Open", item={"file": str(path)}, options={"resume": resume})
        return self.wait_for("Player.OnAVStart", timeout)["player"]["playerid"]

    def stop(self, player_id):
        self.request("Player.Stop", playerid=player_id)
        assert self.wait_for("Player.OnStop")["end"] is False

    def wait_until(self, check, timeout=8):
        end = monotonic() + timeout
        while not check():
            assert monotonic() < end, "Kodi did not reach the expected state"
            sleep(0.05)

    def window(self):
        return self.call("GUI.GetProperties", properties=["currentwindow"])["currentwindow"]["id"]

    def focused(self, control):
        field = f"Control.HasFocus({control})"
        return self.call("XBMC.GetInfoBooleans", booleans=[field])[field]

    def label(self, field="ListItem.Label"):
        return self.call("XBMC.GetInfoLabels", labels=[field])[field]

    def navigate(self, direction, check):
        for _ in range(24):
            if check():
                return
            self.call("Input." + direction)
            sleep(0.1)
        raise AssertionError(f"Could not reach control in window {self.window()}, label={self.label()!r}")

    def start(self):
        return self.wait_for("Player.OnAVStart")["player"]["playerid"]

    def replacement(self, timeout=10):
        for message in self.messages(timeout):
            event = message.get("method")
            if event == "Player.OnStop":
                raise AssertionError(f"Replacement stopped playback: {message['params']['data']}")
            if event == "Player.OnAVStart":
                return message["params"]["data"]["player"]["playerid"]

    def seek(self, player_id, seconds):
        self.request("Player.Seek", playerid=player_id, value={"time": {"seconds": seconds}})
        self.wait_for("Player.OnSeek")
        self.wait_until(lambda: abs(self.position(player_id) - seconds) <= 1)

    def position(self, player_id):
        value = self.call("Player.GetProperties", playerid=player_id, properties=["time"])["time"]
        return value["minutes"] * 60 + value["seconds"]

    def width(self, player_id):
        return self.call("Player.GetProperties", playerid=player_id,
                         properties=["currentvideostream"])["currentvideostream"]["width"]

    def select_another(self):
        self.call("Input.ExecuteAction", action="osd")
        self.wait_until(lambda: self.window() == 12901)
        focused = "Control.HasFocus(70049)"
        for _ in range(14):
            if self.call("XBMC.GetInfoBooleans", booleans=[focused])[focused]:
                self.call("Input.Select")
                return
            self.call("Input.Right")
            sleep(0.08)
        raise AssertionError("Could not reach Try another source with the arrow keys")

    def try_another(self):
        self.select_another()
        return self.replacement()

    def tracks(self, player_id, audio, width, subtitle=None):
        def selected():
            props = self.call("Player.GetProperties", playerid=player_id,
                              properties=["currentaudiostream", "currentvideostream",
                                          "currentsubtitle", "subtitleenabled"])
            return (props["currentaudiostream"]["language"] == audio
                    and props["currentvideostream"]["width"] == width
                    and (subtitle is None or
                         (props["subtitleenabled"] and props["currentsubtitle"]["language"] == subtitle
                          and props["currentsubtitle"]["isforced"] is False)))
        self.wait_until(selected)


def make_language_media(root):
    root.mkdir()
    subtitle = root / "fixture.srt"
    subtitle.write_text("1\n00:00:00,000 --> 00:00:39,000\nSynthetic dialogue\n",
                        encoding="utf-8")
    for name, dual in (("dual.mkv", True), ("JPN-full-subs.mkv", False)):
        width = 240 if dual else 160
        command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                   "-i", f"color=c=black:s={width}x136:r=10", "-f", "lavfi",
                   "-i", "sine=frequency=440", "-f", "lavfi", "-i", "sine=frequency=880",
                   "-i", str(subtitle), "-map", "0:v", "-map", "1:a", "-map", "3:s",
                   "-metadata:s:a:0", "language=eng", "-disposition:a:0", "default",
                   "-metadata:s:s:0", "language=spa", "-disposition:s:0", "forced"]
        if dual:
            command += ["-map", "2:a", "-map", "3:s", "-metadata:s:a:1", "language=jpn",
                        "-metadata:s:s:1", "language=eng", "-disposition:s:1", "0"]
        subprocess.run(command + ["-t", "40", "-c:v", "mpeg4", "-c:a", "aac",
                                  "-c:s", "srt", str(root / name)], check=True)


def language_check(userdata, port, phase):
    root = userdata / "appliance"
    library = root / "library"
    named = library / "edit"
    first = named / "episode-tmdb-900000021.s01e01.mkv"
    next_episode = library / "episode-tmdb-900000022.s01e02.mkv"
    anime_movie = library / "movie-tmdb-900000023.mkv"
    dub_only = library / "movie-tmdb-900000024.mkv"
    only_dual = library / "movie-tmdb-900000026.mkv"
    dual = root / "fixtures" / "dual.mkv"
    misleading = root / "fixtures" / "JPN-full-subs.mkv"
    if phase in ("first", "second-profile"):
        named.mkdir(parents=True)
        for path in (first, next_episode, anime_movie, dub_only, only_dual):
            path.touch()
        make_language_media(dual.parent)
        alternate = dual.with_name("dual-alternate.mkv")
        alternate.write_bytes(dual.read_bytes())
        series = {"seriesId": "tmdb:900000025", "originalLanguage": "ja-JP",
                  "local": [str(misleading), str(dual)]}
        write_json(root / "index.json", {
            "episode:tmdb:900000021": dict(series, episode="s01e01", query="Pilot Show"),
            "episode:tmdb:900000022": dict(series, episode="s01e02"),
            "movie:tmdb:900000023": dict(series, anime=True, local=[str(dual), str(alternate)]),
            "movie:tmdb:900000024": dict(series, local=[str(misleading)]),
            "movie:tmdb:900000026": dict(series, anime=True, local=[str(dual)]),
        })

    with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
        kodi = Kodi(connection)
        if phase == "second-profile":
            profile = kodi.call("Profiles.GetCurrentProfile")
            assert profile["label"] == "Language fixture second profile", profile
        for setting, value in (("locale.audiolanguage", "English"),
                               ("locale.subtitlelanguage", "Spanish")):
            assert kodi.call("Settings.SetSettingValue", setting=setting, value=value)

        if phase == "restart":
            player = kodi.play(next_episode)
            kodi.tracks(player, "jpn", 240, "eng")
            kodi.stop(player)
            print("Series choice persisted across Kodi restart")
            return

        # Remove all local candidates temporarily so a failed cancellation would reach the provider.
        index = json.loads((root / "index.json").read_text(encoding="utf-8"))
        without_local = dict(index)
        anchor_key = "episode:tmdb:900000021"
        without_local[anchor_key] = dict(index[anchor_key], local=[])
        write_json(root / "index.json", without_local)
        with socket.socket() as provider:
            provider.bind(("127.0.0.1", 0))
            provider.listen()
            provider.settimeout(0.5)
            write_json(root / "providers.json",
                       {"fixture": f"http://127.0.0.1:{provider.getsockname()[1]}"})
            kodi.call("Player.Open", item={"file": str(first)})
            kodi.wait_until(lambda: kodi.window() == 12000)
            kodi.call("Input.Back")
            kodi.wait_until(lambda: kodi.window() != 12000)
            assert kodi.call("Player.GetActivePlayers") == []
            try:
                peer, _ = provider.accept()
            except TimeoutError:
                pass
            else:
                peer.close()
                raise AssertionError("Canceling the language prompt contacted the provider")
        assert not (root / "language").exists()
        write_json(root / "index.json", index)
        (root / "providers.json").unlink()
        if phase == "second-profile":
            print("Second Kodi profile prompted again for the same series, and Back canceled")
            return

        kodi.call("Player.Open", item={"file": str(first)})
        kodi.wait_until(lambda: kodi.window() == 12000)
        kodi.call("Input.Select")
        player = kodi.start()
        kodi.tracks(player, "jpn", 240, "eng")
        kodi.stop(player)
        player = kodi.play(next_episode)
        kodi.tracks(player, "jpn", 240, "eng")
        kodi.stop(player)
        print("Back canceled without opening or discovery. Sub skipped the misleading file, "
              "used full English subtitles instead of forced Spanish, and carried across episodes")

        assert kodi.call("Settings.SetSettingValue", setting="appliance.movielanguage", value="sub")
        assert kodi.call("Settings.SetSettingValue", setting="appliance.animelanguage", value="dub")
        assert kodi.call("Settings.GetSettingValue", setting="appliance.quality")["value"] == "balanced"
        assert kodi.call("Settings.SetSettingValue", setting="appliance.quality", value="high")
        assert kodi.call("Settings.SetSettingValue", setting="appliance.quality", value="data")
        subtitle_properties = ["subtitleenabled", "currentsubtitle"]
        player = kodi.play(dual)
        ordinary = kodi.call("Player.GetProperties", playerid=player, properties=subtitle_properties)
        kodi.stop(player)
        player = kodi.play(anime_movie)
        kodi.tracks(player, "eng", 240)
        assert kodi.call("Player.GetProperties", playerid=player, properties=subtitle_properties) == ordinary
        kodi.stop(player)
        player = kodi.play(only_dual)
        kodi.tracks(player, "eng", 240)
        kodi.select_another()
        kodi.wait_until(lambda: kodi.window() == 12000)
        heading = "Control.GetLabel(1)"
        kodi.wait_until(lambda: kodi.call("XBMC.GetInfoLabels", labels=[heading])[heading]
                        == "No playable source supplied the requested tracks.")
        kodi.call("Input.Back")
        kodi.wait_until(lambda: kodi.window() != 12000)
        for accept in (False, True):
            kodi.call("Player.Open", item={"file": str(dub_only)})
            kodi.wait_until(lambda: kodi.window() == 12000)
            kodi.call("Input.Select" if accept else "Input.Back")
            if accept:
                player = kodi.start()
                kodi.tracks(player, "eng", 160)
                kodi.stop(player)
            else:
                kodi.wait_until(lambda: kodi.window() != 12000)
                assert kodi.call("Player.GetActivePlayers") == []
        print("Anime film used the Anime setting. Exhaustion offered Dub and honored decline")
        assert kodi.call("Settings.SetSettingValue", setting="appliance.animelanguage", value="ask")
        kodi.call("Player.Open", item={"file": str(anime_movie)})
        kodi.wait_until(lambda: kodi.window() == 12000)
        kodi.call("Input.Down")
        sleep(0.1)
        kodi.call("Input.Select")
        player = kodi.start()
        kodi.tracks(player, "eng", 240)
        kodi.stop(player)

        def open_editor():
            kodi.call("GUI.ActivateWindow", window="videos", parameters=[str(named), "return"])
            kodi.wait_until(lambda: kodi.window() == 10025)
            for _ in range(3):
                label = kodi.call("XBMC.GetInfoLabels", labels=["ListItem.Filename"])
                if label["ListItem.Filename"] == first.name:
                    break
                kodi.call("Input.Down")
                sleep(0.1)
            else:
                raise AssertionError(f"Unable to focus {first.name}: {label}")
            kodi.call("Input.ContextMenu")
            kodi.wait_until(lambda: kodi.window() == 10106)
            controls = [f"Control.GetLabel({i})" for i in range(1001, 1031)]
            labels = kodi.call("XBMC.GetInfoLabels", labels=controls)
            offset = [labels[control] for control in controls].index("Playback language")
            for _ in range(offset):
                kodi.call("Input.Down")
                sleep(0.1)
            kodi.call("Input.Select")
            kodi.wait_until(lambda: kodi.window() == 12000)

        open_editor()
        kodi.call("Input.Down")
        sleep(0.1)
        kodi.call("Input.Select")
        kodi.wait_until(lambda: kodi.window() != 12000)
        player = kodi.play(next_episode)
        kodi.tracks(player, "eng", 160)
        kodi.stop(player)
        open_editor()
        kodi.call("Input.Down")
        sleep(0.1)
        kodi.call("Input.Select")
        kodi.wait_until(lambda: kodi.window() != 12000)
        kodi.wait_until(lambda: sorted(path.name for path in (root / "language").iterdir())
                        == ["movie-tmdb-900000023"])
        kodi.call("Player.Open", item={"file": str(next_episode)})
        kodi.wait_until(lambda: kodi.window() == 12000)
        kodi.call("Input.Select")
        player = kodi.start()
        kodi.tracks(player, "jpn", 240, "eng")
        kodi.stop(player)
        print("Changed and cleared the series choice from the title context menu")


def recovery_check(userdata, port, phase):
    root = userdata / "appliance"
    library = root / "library"
    episode = library / "episode-tmdb-900000002.s01e02.mkv"
    entry = {"title": "Return", "episode": "s01e02", "query": "Pilot Show", "runtimeSeconds": 40}
    index = {"episode:tmdb:900000002": entry}
    if phase == "first":
        library.mkdir(parents=True)
        episode.touch()
        write_json(root / "index.json", index)
    with tempfile.TemporaryDirectory() as folder, ThreadingHTTPServer(
            ("127.0.0.1", 0), Provider) as provider:
        media = Path(folder) / "test.mp4"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                        "-i", "testsrc2=size=240x136:rate=10", "-t", "40", "-c:v", "mpeg4",
                        "-g", "10", "-movflags", "+faststart", str(media)], check=True)
        provider.media = media.read_bytes()
        provider.calls = Counter()
        provider.resolved_routes = []
        provider.mode = "break" if phase == "first" else "clean"
        provider.break_read = Event()
        provider.discovery_started = Event()
        provider.discovery_replied = Event()
        provider.release_discovery = Event()
        provider.release_discovery.set()
        write_json(root / "providers.json", {"fixture": f"http://127.0.0.1:{provider.server_port}"})
        Thread(target=provider.serve_forever, daemon=True).start()
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
                kodi = Kodi(connection)
                if phase == "restart":
                    player_id = kodi.play(episode, resume=True)
                    position = kodi.position(player_id)
                    assert 14 <= position <= 26, position
                    database, = (userdata / "Database").glob("MyVideos*.db")
                    with sqlite3.connect(database) as db:
                        saved, = db.execute(
                            "SELECT timeInSeconds FROM bookmark JOIN files USING (idFile) "
                            "WHERE strFilename = ? AND type = 1", (episode.name,)
                        ).fetchone()
                    assert abs(position - saved) <= 1.5, (position, saved)
                    assert provider.resolved_routes == [(20, 4)], provider.resolved_routes
                    kodi.seek(player_id, 38)
                    assert kodi.wait_for("Player.OnStop")["end"] is True
                    assert provider.resolved_routes == [(20, 4)], provider.resolved_routes
                    print(f"Restart preferred the last working file at {position}s. Clean EOF ended.")
                else:
                    player_id = kodi.play(media)
                    kodi.call("Input.ExecuteAction", action="osd")
                    kodi.wait_until(lambda: kodi.window() == 12901)
                    visible = "Control.IsVisible(70049)"
                    assert not kodi.call("XBMC.GetInfoBooleans", booleans=[visible])[visible]
                    kodi.call("Input.Back")
                    kodi.wait_until(lambda: kodi.window() != 12901)
                    kodi.seek(player_id, 38)
                    assert kodi.wait_for("Player.OnStop")["end"] is True
                    assert not provider.calls
                    player_id = kodi.play(episode)
                    kodi.seek(player_id, 15)
                    provider.break_read.set()
                    player_id = kodi.replacement(35)
                    position = kodi.position(player_id)
                    assert 14 <= position <= 26, position
                    assert provider.resolved_routes == [(11, 4), (20, 4)], provider.resolved_routes
                    kodi.stop(player_id)
                    print(f"Hard HTTP read failure resumed a distinct file at {position}s.")
                    return

                # Exercise OSD and quick replay on a separate title with its own bookmark.
                provider.mode = "clean"
                provider.resolved_routes.clear()
                alternate = library / "episode-tmdb-900000012.s01e02.mkv"
                alternate.touch()
                local = Path(folder) / "local.mkv"
                subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                                "-i", "color=size=160x90:rate=10", "-t", "40",
                                "-c:v", "mpeg4", "-g", "10", str(local)], check=True)
                index["episode:tmdb:900000012"] = dict(entry, local=[str(local), str(media)])
                write_json(root / "index.json", index)
                player_id = kodi.play(alternate)
                assert kodi.width(player_id) == 160
                kodi.seek(player_id, 14)
                player_id = kodi.try_another()
                assert kodi.width(player_id) == 240
                assert abs(kodi.position(player_id) - 14) <= 2
                assert not provider.resolved_routes
                player_id = kodi.try_another()
                assert provider.resolved_routes == [(11, 4)], provider.resolved_routes
                player_id = kodi.try_another()
                assert provider.resolved_routes == [(11, 4), (20, 4)], provider.resolved_routes
                kodi.stop(player_id)
                player_id = kodi.play(alternate)
                assert kodi.width(player_id) == 160
                assert abs(kodi.position(player_id) - 14) <= 3
                kodi.stop(player_id)
                player_id = kodi.play(alternate)
                assert kodi.width(player_id) == 240
                assert abs(kodi.position(player_id) - 14) <= 3
                kodi.stop(player_id)
                print("OSD replaced local and remote sources. Quick Play skipped the stopped file.")

                capped = library / "episode-tmdb-900000013.s01e02.mkv"
                capped.touch()
                missing = [str(Path(folder) / f"missing-{i}.mkv") for i in range(17)]
                index["episode:tmdb:900000013"] = dict(entry, local=missing + [str(local)])
                write_json(root / "index.json", index)
                counts = provider.calls.copy()
                kodi.call("Player.Open", item={"file": str(capped)})
                kodi.wait_until(lambda: kodi.window() == 12000)
                assert kodi.call("Player.GetActivePlayers") == []
                assert provider.calls == counts
                # Retry is the first row, the final playable local is the third.
                kodi.call("Input.Down")
                sleep(0.1)
                kodi.call("Input.Down")
                sleep(0.1)
                kodi.request("Input.Select")
                player_id = kodi.start()
                assert kodi.width(player_id) == 160
                kodi.stop(player_id)
                assert provider.calls == counts
                print("The attempt cap stopped before discovery, and manual local selection worked.")

                retry = library / "movie-tmdb-900000014.mkv"
                retry.touch()
                repaired = Path(folder) / "repaired.mkv"
                index["movie:tmdb:900000014"] = {"local": [str(repaired)]}
                write_json(root / "index.json", index)
                kodi.call("Player.Open", item={"file": str(retry)})
                kodi.wait_until(lambda: kodi.window() == 12000)
                kodi.call("Input.Back")
                kodi.wait_until(lambda: kodi.window() != 12000)
                assert kodi.call("Player.GetActivePlayers") == []
                kodi.call("Player.Open", item={"file": str(retry)})
                kodi.wait_until(lambda: kodi.window() == 12000)
                repaired.write_bytes(local.read_bytes())
                kodi.request("Input.Select")
                player_id = kodi.start()
                kodi.stop(player_id)
                print("Back ended exhaustion. Intentional Retry opened a repaired local source.")
        finally:
            provider.break_read.set()
            provider.shutdown()


def library_check(userdata, port, phase):
    root = userdata / "appliance"
    movies = root / "library" / "movies"
    shows = root / "library" / "shows"
    show = shows / "Pilot Show"
    movie = movies / "movie-tmdb-900000001.mkv"
    episode = show / "episode-tmdb-900000002.s01e02.mkv"
    titles = (("movie", "Pilot Movie", movie, 15), ("episode", "Return", episode, 25))
    media = root / "fixtures" / "dual.mkv"
    alternate = media.with_name("alternate.mkv")
    database, = (userdata / "Database").glob("MyVideos*.db")

    if phase == "first":
        root.mkdir()
        movies.mkdir(parents=True)
        show.mkdir(parents=True)
        movie.touch()
        episode.touch()
        movie.with_suffix(".nfo").write_text(
            '<movie><title>Pilot Movie</title><uniqueid type="tmdb" default="true">'
            '900000001</uniqueid></movie>', encoding="utf-8")
        (show / "tvshow.nfo").write_text(
            '<tvshow><title>Pilot Show</title><uniqueid type="tmdb" default="true">'
            '900000003</uniqueid></tvshow>', encoding="utf-8")
        episode.with_suffix(".nfo").write_text(
            '<episodedetails><title>Return</title><season>1</season><episode>2</episode>'
            '<uniqueid type="tmdb" default="true">900000002</uniqueid></episodedetails>',
            encoding="utf-8")
        make_language_media(media.parent)
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(media),
                        "-map", "0", "-c", "copy", "-vf", "scale=320:180", "-c:v", "mpeg4",
                        "-g", "10", str(alternate)], check=True)
        entry = {"local": [str(media), str(alternate)], "originalLanguage": "ja"}
        write_json(root / "index.json", {"movie:tmdb:900000001": entry,
                                        "episode:tmdb:900000002": dict(entry, episode="s01e02")})
        # Configure only the source's scraper rows. Kodi scans and creates every title/file row.
        with sqlite3.connect(database) as db:
            db.executemany(
                "INSERT INTO path (strPath, strContent, strScraper, scanRecursive, useFolderNames, "
                "strSettings, noUpdate, exclude, allAudio) "
                "VALUES (?, ?, 'metadata.local', 0, 0, '', 0, 0, 0)",
                ((str(movies) + "/", "movies"), (str(shows) + "/", "tvshows")))

    with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
        kodi = Kodi(connection)
        if phase == "first":
            for folder in (movies, shows):
                kodi.request("VideoLibrary.Scan", directory=str(folder) + "/", showdialogs=False)
                kodi.wait_for("VideoLibrary.OnScanFinished", 20)
            for setting, value in (("locale.audiolanguage", "English"),
                                   ("locale.subtitlelanguage", "Spanish"),
                                   ("myvideos.selectaction", 2), ("myvideos.playaction", 2),
                                   ("myvideos.extractthumb", False), ("myvideos.extractflags", False)):
                assert kodi.call("Settings.SetSettingValue", setting=setting, value=value)

        def label():
            return kodi.call("XBMC.GetInfoLabels", labels=["ListItem.Label"])["ListItem.Label"]

        def video_title():
            return kodi.call("XBMC.GetInfoLabels", labels=["ListItem.Title"])["ListItem.Title"]

        def focus(condition, direction):
            for _ in range(16):
                if condition():
                    return
                kodi.call("Input." + direction)
                sleep(0.1)
            raise AssertionError(f"Could not focus the requested item, current label={label()!r}")

        def open_videos(path):
            kodi.call("GUI.ActivateWindow", window="videos", parameters=[path])
            kodi.wait_until(lambda: kodi.window() == 10025)

        def control_focused(control):
            condition = f"Control.HasFocus({control})"
            return kodi.call("XBMC.GetInfoBooleans", booleans=[condition])[condition]

        def open_home_control(control):
            kodi.call("Input.Home")
            kodi.wait_until(lambda: kodi.window() == 10000)
            if kodi.focused(10):
                kodi.call("Input.Select")
                kodi.wait_until(lambda: kodi.focused(11))
            if kodi.focused(20):
                kodi.call("Input.Left")
            kodi.navigate("Down", lambda: kodi.focused(control))

        def bookmark(anchor):
            with sqlite3.connect(database) as db:
                row = db.execute(
                    "SELECT timeInSeconds FROM bookmark JOIN files USING (idFile) "
                    "WHERE strFilename = ? AND type = 1", (anchor.name,)
                ).fetchone()
            return row[0] if row else 0

        def check_resume(anchor, expected, seek_to):
            player = kodi.start()
            kodi.tracks(player, "jpn", 320, "eng")

            def at_bookmark():
                time = kodi.call("Player.GetProperties", playerid=player, properties=["time"])["time"]
                position = 60 * time["minutes"] + time["seconds"] + time["milliseconds"] / 1000
                return abs(position - expected) <= 2

            kodi.wait_until(at_bookmark, timeout=3)
            kodi.seek(player, seek_to)
            # A title switch saves progress without arming the quick replay window for this title.
            kodi.stop(kodi.play(media))
            kodi.wait_until(lambda: abs(bookmark(anchor) - seek_to) <= 2)

        show_item, = kodi.call("VideoLibrary.GetTVShows")["tvshows"]
        series = f"videodb://tvshows/titles/{show_item['tvshowid']}/"
        for kind, title, anchor, seek_to in titles:
            library_item, = kodi.call("VideoLibrary.GetMovies" if kind == "movie" else
                                      "VideoLibrary.GetEpisodes", properties=["file", "uniqueid"])[kind + "s"]
            assert library_item["file"] == str(anchor), library_item
            assert library_item["uniqueid"]["tmdb"] == anchor.name.split("-")[2].split(".")[0]
            library_id = library_item[kind + "id"]
            browse = "videodb://movies/titles/" if kind == "movie" else series + "-1/"

            def detail():
                open_videos(browse)
                focus(lambda: video_title() == title, "Down")
                kodi.call("Input.ExecuteAction", action="info")
                kodi.wait_until(lambda: kodi.window() == 12003)
                kodi.request("Input.Select")

            if phase == "first":
                detail()
                kodi.wait_until(lambda: kodi.window() == 12000)
                kodi.call("Input.Back")
                kodi.wait_until(lambda: kodi.window() != 12000)
                assert kodi.call("Player.GetActivePlayers") == []
                detail()
                kodi.wait_until(lambda: kodi.window() == 12000)
                kodi.request("Input.Select")
                player = kodi.start()
                kodi.tracks(player, "jpn", 240, "eng")
                kodi.seek(player, seek_to)
                player = kodi.try_another()
                kodi.tracks(player, "jpn", 320, "eng")
                kodi.wait_until(lambda: abs(kodi.position(player) - seek_to) <= 1)
                kodi.stop(player)
                kodi.wait_until(lambda: abs(bookmark(anchor) - seek_to) <= 2)
                assert kodi.call("Favourites.AddFavourite", type="media", title=title,
                                 path=str(anchor)) == "OK"
                print(f"{kind}: scanned NFO, detail Play, Ask/Back, Sub, OSD switch and title bookmark")
                continue

            saved = bookmark(anchor)
            assert abs(saved - seek_to) <= 2, (kind, saved)
            kodi.request("Player.Open", item={kind + "id": library_id}, options={"resume": True})
            check_resume(anchor, saved, seek_to)
            print(f"{kind}: library ID resumed the saved source, tracks and {saved:.1f}s bookmark")
            for route in ("detail", "favorite", "in-progress"):
                saved = bookmark(anchor)
                if route == "detail":
                    detail()
                elif route == "favorite":
                    open_home_control(12)
                    kodi.call("Input.Select")
                    kodi.wait_until(lambda: kodi.window() == 10060)
                    focus(lambda: label() == title, "Right")
                    kodi.request("Input.Select")
                else:
                    open_home_control(13)
                    kodi.call("Input.Select")
                    kodi.wait_until(lambda: kodi.focused(20))
                    kodi.navigate("Down", lambda: kodi.label("Container(20).ListItem.Title") == title)
                    kodi.call("Input.Select")
                    kodi.wait_until(lambda: kodi.window() == 12003)
                    kodi.request("Input.Select")
                check_resume(anchor, saved, seek_to)
                print(f"{kind}: Estuary {route} resumed the saved source, tracks and {saved:.1f}s bookmark")


def home_check(userdata, port, phase):
    root = userdata / "appliance"
    folder = Path.home() / "M4 Fixture Movies"
    media = folder / "A Fixture Movie.mkv"
    title = media.stem
    if phase == "first":
        assert not root.exists(), root
        folder.mkdir()
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                        "-i", "color=red:size=160x90:rate=10", "-t", "40", "-c:v", "mpeg4",
                        "-g", "10", str(media)], check=True)
        media.with_suffix(".nfo").write_text(
            f'<movie><title>{title}</title><uniqueid type="tmdb" default="true">'
            '900000004</uniqueid></movie>', encoding="utf-8")

    with (ThreadingHTTPServer(("127.0.0.1", 0), Provider) as provider,
          socket.create_connection(("127.0.0.1", port), timeout=10) as connection):
        provider.calls = Counter()
        Thread(target=provider.serve_forever, daemon=True).start()
        kodi = Kodi(connection)
        anchor = root / "library" / "movies" / "movie-tmdb-900000004.mkv"
        row = root / "index" / "movie-tmdb-900000004.json"

        def press(control=None, window=None):
            if control is not None:
                kodi.wait_until(lambda: kodi.focused(control))
            kodi.request("Input.Select")
            if window is not None:
                kodi.wait_until(lambda: kodi.window() == window)

        def movies():
            return kodi.call("VideoLibrary.GetMovies", properties=["file", "uniqueid", "resume"]).get(
                "movies", [])

        def result_focused():
            return kodi.focused(20) and kodi.label("Container(20).ListItem.Label") == title

        def add_folder():
            kodi.navigate("Down", lambda: kodi.focused(14))
            press(14, 12000)
            assert kodi.label() == "Media folders"
            press()
            kodi.wait_until(lambda: kodi.label() == "Add folder")
            press(window=10126)
            kodi.navigate("Down", lambda: kodi.label() == "Home folder")
            press()
            kodi.navigate("Down", lambda: kodi.label() == folder.name)
            press()
            kodi.wait_until(lambda: str(folder) in kodi.label("Control.GetLabel(412)"))
            kodi.call("Input.Left")
            press(413)
            kodi.wait_for("VideoLibrary.OnScanFinished", 20)

        try:
            kodi.wait_until(lambda: kodi.window() == 10000)
            if phase == "first":
                press(10, 10000)
                kodi.navigate("Down", lambda: kodi.focused(13))
                press(13, 10000)
                kodi.wait_until(lambda: kodi.label("Window.Property(ApplianceEmpty)"))
                kodi.call("Input.Back")
                kodi.wait_until(lambda: not kodi.label("Window.Property(ApplianceEmpty)"))
            else:
                kodi.wait_until(lambda: kodi.focused(11))
                saved, = movies()

            add_folder()
            movie, = movies()
            assert movie["file"] == str(anchor) and movie["uniqueid"]["tmdb"] == "900000004", movie
            assert anchor.stat().st_size == 0 and anchor.with_suffix(".nfo").is_file()
            entry = json.loads(row.read_text(encoding="utf-8"))
            assert entry["local"] == [str(media)], entry
            # Give discovery a valid route so removing the media tests the local-only gate.
            write_json(root / "providers.json", {"fixture": f"http://127.0.0.1:{provider.server_port}"})
            write_json(row, dict(entry, query="Pilot Show"))

            if phase == "first":
                kodi.navigate("Down", lambda: kodi.focused(11))
                press(11, 10103)
                kodi.call("Input.Right")
                assert kodi.label("Control.GetLabel(140)").lower() == "a"
                press(140)
                kodi.navigate("Right", lambda: kodi.focused(300))
                press(300, 10000)
                kodi.wait_until(result_focused)
                press(20, 12003)
                kodi.call("Input.Back")
                kodi.wait_until(lambda: kodi.window() == 10000 and kodi.focused(20))
                press(20, 12003)
                press(8)
                player = kodi.start()
                assert kodi.width(player) == 160
                kodi.seek(player, 15)
                kodi.stop(player)
                kodi.wait_until(lambda: abs(movies()[0]["resume"]["position"] - 15) <= 2)
            else:
                assert movie["movieid"] == saved["movieid"] and movie["resume"] == saved["resume"]
                kodi.navigate("Down", lambda: kodi.focused(13))
                press(13, 10000)
                kodi.wait_until(result_focused)
                press(20, 12003)
                press(8)
                player = kodi.start()
                assert abs(kodi.position(player) - saved["resume"]["position"]) <= 2
                kodi.stop(player)

                missing = media.with_suffix(".missing")
                media.rename(missing)
                try:
                    kodi.call("Player.Open", item={"movieid": movie["movieid"]})
                    kodi.wait_until(lambda: kodi.window() == 12000)
                    assert "Retry" in kodi.label()
                    kodi.call("Input.Select")
                    sleep(0.6)
                    kodi.wait_until(lambda: kodi.window() == 12000)
                    assert not provider.calls, provider.calls
                    kodi.call("Input.Back")
                    kodi.wait_until(lambda: kodi.window() != 12000)
                finally:
                    missing.rename(media)
                kodi.stop(kodi.play(media))
            assert not provider.calls, provider.calls
            print(f"Home {phase}: indexed movie, title detail and bookmark, zero provider requests")
        finally:
            provider.shutdown()


def play_to_frame(kodi, path, channel, resume=False):
    # Sample the middle pixel of the Xvfb output, where the synthetic solid video appears.
    from ctypes import CDLL, c_char_p, c_int, c_ulong, c_void_p

    x11 = CDLL("libX11.so.6")
    for name, result, args in (
        ("XOpenDisplay", c_void_p, [c_char_p]),
        ("XDefaultRootWindow", c_ulong, [c_void_p]),
        ("XGetImage", c_void_p, [c_void_p, c_ulong, c_int, c_int, c_int, c_int, c_ulong, c_int]),
        ("XGetPixel", c_ulong, [c_void_p, c_int, c_int]),
        ("XDestroyImage", c_int, [c_void_p]), ("XCloseDisplay", c_int, [c_void_p]),
    ):
        function = getattr(x11, name)
        function.restype, function.argtypes = result, args
    display = x11.XOpenDisplay(None)
    assert display, "Start Kodi and the frame check on the same 1280x720x24 Xvfb display"
    try:
        root = x11.XDefaultRootWindow(display)

        def first_frame():
            image = x11.XGetImage(display, root, 640, 360, 1, 1, ~0, 2)
            assert image
            pixel = x11.XGetPixel(image, 0, 0)
            x11.XDestroyImage(image)
            red, green, blue = ((pixel >> shift) & 255 for shift in (16, 8, 0))
            colors = (red, blue) if channel == "red" else (blue, red)
            return colors[0] > 180 and max(green, colors[1]) < 50

        assert not first_frame(), "Synthetic frame was already visible before Play"
        started = monotonic()
        kodi.request("Player.Open", item={"file": str(path)}, options={"resume": resume})
        while not first_frame():
            assert monotonic() - started < 25, f"No {channel} video frame reached Xvfb"
            sleep(0.005)
        elapsed = monotonic() - started
        return kodi.start(), elapsed
    finally:
        x11.XCloseDisplay(display)


def run(userdata, port, measure_frames=False):
    root = userdata / "appliance"
    root.mkdir()  # Fail before touching an existing appliance configuration.
    library = root / "library"
    library.mkdir()
    movie = library / "movie-tmdb-900000001.mkv"
    episode = library / "episode-tmdb-900000002.s01e02.mkv"
    for anchor in (movie, episode):
        anchor.touch()

    with tempfile.TemporaryDirectory(prefix="kodi-appliance-") as folder:
        local, remote = (Path(folder) / name for name in ("local.mkv", "remote.mkv"))
        broken = [Path(folder) / f"broken-{i}.mkv" for i in range(4)]
        for path in broken:
            path.write_bytes(b"synthetic corrupt media")
        for media, color, resolution in ((local, "red", "160x90"), (remote, "blue", "240x136")):
            subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                            "-i", f"color={color}:size={resolution}:rate=10", "-t", "40",
                            "-c:v", "mpeg4", "-g", "10", str(media)], check=True)

        with ThreadingHTTPServer(("127.0.0.1", 0), Provider) as provider:
            provider.calls = Counter()
            provider.resolved_routes = []
            provider.resolve_seconds = []
            provider.mode = "refresh"
            provider.media = remote.read_bytes()
            provider.discovery_started = Event()
            provider.release_discovery = Event()
            provider.discovery_replied = Event()
            provider.release_discovery.set()
            Thread(target=provider.serve_forever, daemon=True).start()
            try:
                write_json(root / "providers.json", {"fixture": f"http://127.0.0.1:{provider.server_port}"})
                index = {
                    "movie:tmdb:900000001": {"local": [str(path) for path in broken] + [
                        {"path": str(local), "bytes": local.stat().st_size, "format": "WEB-DL"}],
                                              "query": "Pilot Movie"},
                    "episode:tmdb:900000002": {"title": "Return", "episode": "s01e02",
                                               "query": "Pilot Show", "runtimeSeconds": 40,
                                               "local": [str(local)]},
                }
                write_json(root / "index.json", index)

                with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
                    kodi = Kodi(connection)
                    kodi.stop(kodi.play(local))
                    kodi.stop(kodi.play(movie))
                    if measure_frames:
                        player_id, frame_delay = play_to_frame(kodi, episode, "red")
                        print(f"Local Play to first visible Xvfb frame: {frame_delay:.3f}s")
                    else:
                        player_id = kodi.play(episode)
                    assert not provider.calls, provider.calls

                    kodi.request("Player.Seek", playerid=player_id, value={"time": {"seconds": 15}})
                    kodi.wait_for("Player.OnSeek")
                    deadline = monotonic() + 5
                    while True:
                        position = kodi.call("Player.GetProperties", playerid=player_id,
                                             properties=["time"])["time"]
                        if position["seconds"] >= 13:
                            break
                        assert monotonic() < deadline, position
                        sleep(0.1)
                    kodi.stop(player_id)
                    assert not provider.calls, provider.calls
                    print("Ordinary playback, fifth local with zero provider requests, and Stop passed",
                          flush=True)

                    with socket.socket() as disconnected:
                        disconnected.bind(("127.0.0.1", 0))
                        disconnected.listen(4)  # Accept TCP handshakes without answering SMB.
                        smb_port = disconnected.getsockname()[1]
                        index["episode:tmdb:900000002"]["local"] = [
                            f"smb://127.0.0.1:{smb_port}/share/absent.mkv"
                        ]
                        write_json(root / "index.json", index)
                        fallback_start = monotonic()
                        if measure_frames:
                            player_id, frame_delay = play_to_frame(kodi, episode, "blue", resume=True)
                            print(f"Silent SMB and refreshed link to visible Xvfb frame: {frame_delay:.3f}s")
                        else:
                            player_id = kodi.play(episode, resume=True, timeout=25)
                    discovery_delay = provider.discovered_at - fallback_start
                    assert 4 <= discovery_delay < 20, discovery_delay
                    properties = kodi.call("Player.GetProperties", playerid=player_id,
                                           properties=["time", "currentvideostream"])
                    assert properties["time"]["seconds"] >= 13, properties["time"]
                    assert properties["currentvideostream"]["width"] == 240, properties
                    assert provider.calls["discover"] == 1, provider.calls
                    assert provider.calls["list"] == 1, provider.calls
                    assert provider.resolved_routes == [(11, 4), (11, 4)], provider.resolved_routes
                    assert provider.calls["expired"] >= 1, provider.calls
                    assert provider.calls["playing"] >= 1, provider.calls
                    kodi.wait_until(lambda: kodi.position(player_id) >= 14)
                    kodi.stop(player_id)
                    print(f"Silent SMB connection to discovery in {discovery_delay:.3f}s", flush=True)
                    if measure_frames:
                        print(f"Fallback discovery from search through list response: "
                              f"{1000 * (provider.listed_at - provider.discovered_at):.2f}ms")
                        for route, elapsed in provider.resolve_seconds:
                            print(f"Fallback resolver request {route} through response: "
                                  f"{1000 * elapsed:.2f}ms")

                    index["episode:tmdb:900000002"]["local"] = [str(local)]
                    write_json(root / "index.json", index)
                    player_id = kodi.play(episode, resume=True)
                    properties = kodi.call("Player.GetProperties", playerid=player_id,
                                           properties=["time", "currentvideostream"])
                    assert properties["time"]["seconds"] >= 13, properties["time"]
                    assert properties["currentvideostream"]["width"] == 160, properties
                    assert provider.calls["discover"] == 1, provider.calls
                    assert provider.calls["resolve"] == 2, provider.calls
                    kodi.stop(player_id)
                    print(f"Exact file ({len(provider.media)} bytes), expired link and resume passed")

                    index["episode:tmdb:900000002"]["local"] = [str(Path(folder) / "missing.mkv")]
                    write_json(root / "index.json", index)
                    for mode, expected in (("alternate", [(11, 4), (11, 4), (12, 7)]),
                                           ("next-file", [(12, 7), (20, 4)])):
                        provider.mode = mode
                        provider.resolved_routes.clear()
                        discovered = provider.calls["discover"]
                        listed = provider.calls["list"]
                        player_id = kodi.play(episode)
                        if mode == "alternate":
                            kodi.stop(kodi.play(local))
                        else:
                            kodi.stop(player_id)
                        assert provider.calls["discover"] == discovered + 1, provider.calls
                        assert provider.calls["list"] == listed + 1, provider.calls
                        assert provider.resolved_routes == expected, provider.resolved_routes
                    print("Missing local, alternate route, cooldown, and next eligible file passed")

                    index["episode:tmdb:900000002"]["local"] = []
                    write_json(root / "index.json", index)
                    for cancel in ("stop", "replace"):
                        provider.discovery_started.clear()
                        provider.release_discovery.clear()
                        provider.discovery_replied.clear()
                        discovered = provider.calls["discover"]
                        resolved = provider.calls["resolve"]
                        listed = provider.calls["list"]
                        kodi.call("Player.Open", item={"file": str(episode)})
                        assert provider.discovery_started.wait(3)
                        if cancel == "stop":
                            kodi.call("Player.Open", item={"file": str(episode)})
                            kodi.call("Input.ExecuteAction", action="stop")
                            sleep(0.25)
                        else:
                            player_id = kodi.play(local)
                        provider.release_discovery.set()
                        assert provider.discovery_replied.wait(3)
                        sleep(0.25)
                        assert provider.calls["resolve"] == resolved, provider.calls
                        assert provider.calls["list"] == listed, provider.calls
                        assert provider.calls["discover"] == discovered + 1, provider.calls
                        if cancel == "replace":
                            kodi.stop(player_id)
                        else:
                            assert kodi.call("Player.GetActivePlayers") == []
                    print("Duplicate Play, Stop during discovery and switch to ordinary playback passed")
                    if measure_frames:
                        clean = library / "episode-tmdb-900000003.s01e02.mkv"
                        clean.touch()
                        index["episode:tmdb:900000003"] = index["episode:tmdb:900000002"]
                        write_json(root / "index.json", index)
                        provider.mode = "clean"
                        provider.resolve_seconds.clear()
                        player_id, frame_delay = play_to_frame(kodi, clean, "blue")
                        kodi.stop(player_id)
                        print(f"Remote-only Play to first visible Xvfb frame: {frame_delay:.3f}s")
                        print(f"Discovery from search request through list response: "
                              f"{1000 * (provider.listed_at - provider.discovered_at):.2f}ms")
                        for route, elapsed in provider.resolve_seconds:
                            print(f"Resolver request {route} through response: {1000 * elapsed:.2f}ms")
            finally:
                provider.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("userdata", type=Path, help="userdata folder of an isolated Kodi profile")
    parser.add_argument("--port", type=int, default=19090)
    parser.add_argument("--language-phase", choices=["first", "restart", "second-profile"])
    parser.add_argument("--recovery-phase", choices=["first", "restart"])
    parser.add_argument("--library-phase", choices=["first", "restart"])
    parser.add_argument("--home-phase", choices=["first", "restart"])
    parser.add_argument("--measure-frames", action="store_true",
                        help="sample first visible frames on a 1280x720x24 Linux Xvfb display")
    args = parser.parse_args()
    userdata = args.userdata.resolve(strict=True)
    if args.language_phase:
        language_check(userdata, args.port, args.language_phase)
    elif args.recovery_phase:
        recovery_check(userdata, args.port, args.recovery_phase)
    elif args.library_phase:
        library_check(userdata, args.port, args.library_phase)
    elif args.home_phase:
        home_check(userdata, args.port, args.home_phase)
    else:
        run(userdata, args.port, args.measure_frames)
