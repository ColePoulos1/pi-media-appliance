"""Exercise appliance playback against a running Kodi fork with disposable userdata."""

import argparse
import json
import socket
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
        elif url.path == "/v1/api/torrents/requestdl":
            self.server.calls["resolve"] += 1
            assert "token" not in query
            route = (int(query["torrent_id"][0]), int(query["file_id"][0]))
            self.server.resolved_routes.append(route)
            assert route in {(11, 4), (12, 7), (20, 4)}, route
            expires = route == (11, 4) and (
                self.server.mode == "alternate" or self.server.calls["resolve"] == 1)
            source = "expired.mkv" if expires else "playing.mkv"
            if self.server.mode == "next-file" and route[0] != 20:
                reply = {"success": False}
            else:
                reply = {"success": True,
                         "data": f"http://127.0.0.1:{self.server.server_port}/{source}"}
            self.reply(200, json.dumps(reply).encode(), "application/json")
        elif url.path == "/expired.mkv":
            self.server.calls["expired"] += 1
            self.reply(403, b"expired", "text/plain")
        elif url.path == "/playing.mkv":
            self.server.calls["playing"] += 1
            body = self.server.media
            byte_range = self.headers.get("Range", "")
            if byte_range.startswith("bytes="):
                from_byte, _, through_byte = byte_range[6:].partition("-")
                start = int(from_byte)
                if start >= len(body):
                    self.reply(416, b"", "video/x-matroska")
                    return
                end = min(int(through_byte) if through_byte else len(body) - 1, len(body) - 1)
                extent = f"bytes {start}-{end}/{len(body)}"
                self.reply(206, body[start : end + 1], "video/x-matroska",
                           [("Content-Range", extent), ("Accept-Ranges", "bytes")])
            else:
                self.reply(200, body, "video/x-matroska", [("Accept-Ranges", "bytes")])
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
        self.request(method, **params)
        for message in self.messages(10):
            if "id" in message:
                if "error" in message:
                    raise RuntimeError(f"{method}: {message['error']}")
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

    def start(self):
        return self.wait_for("Player.OnAVStart")["player"]["playerid"]

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
    dual = root / "fixtures" / "dual.mkv"
    misleading = root / "fixtures" / "JPN-full-subs.mkv"
    if phase in ("first", "second-profile"):
        named.mkdir(parents=True)
        for path in (first, next_episode, anime_movie, dub_only):
            path.touch()
        make_language_media(dual.parent)
        series = {"seriesId": "tmdb:900000025", "originalLanguage": "ja-JP",
                  "local": [str(misleading), str(dual)]}
        write_json(root / "index.json", {
            "episode:tmdb:900000021": dict(series, episode="s01e01", query="Pilot Show"),
            "episode:tmdb:900000022": dict(series, episode="s01e02"),
            "movie:tmdb:900000023": dict(series, anime=True, local=[str(dual)]),
            "movie:tmdb:900000024": dict(series, local=[str(misleading)]),
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


def run(userdata, port):
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
                    kodi.stop(player_id)
                    print(f"Silent SMB connection to discovery in {discovery_delay:.3f}s", flush=True)

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
                                           ("next-file", [(11, 4), (12, 7), (20, 4)])):
                        provider.mode = mode
                        provider.resolved_routes.clear()
                        discovered = provider.calls["discover"]
                        listed = provider.calls["list"]
                        kodi.stop(kodi.play(episode))
                        assert provider.calls["discover"] == discovered + 1, provider.calls
                        assert provider.calls["list"] == listed + 1, provider.calls
                        assert provider.resolved_routes == expected, provider.resolved_routes
                    print("Missing local, alternate route and next ranked file passed")

                    index["episode:tmdb:900000002"]["local"] = []
                    write_json(root / "index.json", index)
                    provider.discovery_started.clear()
                    provider.release_discovery.clear()
                    provider.discovery_replied.clear()
                    resolved = provider.calls["resolve"]
                    kodi.call("Player.Open", item={"file": str(episode)})
                    assert provider.discovery_started.wait(3)
                    player_id = kodi.play(movie)
                    provider.release_discovery.set()
                    assert provider.discovery_replied.wait(3)
                    sleep(0.25)
                    assert provider.calls["resolve"] == resolved, provider.calls
                    kodi.stop(player_id)
                    print("A late discovery response left the replacement playback alone")
            finally:
                provider.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("userdata", type=Path, help="userdata folder of an isolated Kodi profile")
    parser.add_argument("--port", type=int, default=19090)
    parser.add_argument("--language-phase", choices=["first", "restart", "second-profile"])
    args = parser.parse_args()
    userdata = args.userdata.resolve(strict=True)
    if args.language_phase:
        language_check(userdata, args.port, args.language_phase)
    else:
        run(userdata, args.port)
