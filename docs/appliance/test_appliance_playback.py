"""Exercise Milestone 2 against a running Kodi fork with disposable userdata."""

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
        if url.path == "/discover":
            self.server.calls["discover"] += 1
            self.server.discovered_at = monotonic()
            self.server.discovery_started.set()
            assert self.server.release_discovery.wait(5), "Timed out waiting for the test client"
            assert query["query"] == ["Pilot Show"]
            pack = [
                {"id": 2, "name": "Pilot.Show.S01E02.Wrong.mkv", "size": 1_000_000_000},
                {"id": 3, "name": "Pilot.Show.S01E03.Return.mkv", "size": 1_100_000_000},
                {"id": 4, "name": "Pilot.Show.S01E02.Return.mkv", "size": len(self.server.media)},
            ]
            self.reply(200, json.dumps(pack).encode(), "application/json")
            self.server.discovery_replied.set()
        elif url.path == "/resolve":
            self.server.calls["resolve"] += 1
            assert query["file_id"] == ["4"], query
            source = "expired.mkv" if self.server.calls["resolve"] == 1 else "playing.mkv"
            reply = {"url": f"http://127.0.0.1:{self.server.server_port}/{source}"}
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


def run(userdata, port):
    root = userdata / "appliance"
    root.mkdir()  # Fail before touching an existing appliance configuration.
    library = root / "library"
    library.mkdir()
    movie = library / "movie-tmdb-900000001.mkv"
    episode = library / "episode-tmdb-900000002.s01e02.mkv"
    for anchor in (movie, episode):
        anchor.touch()

    with tempfile.TemporaryDirectory(prefix="kodi-m2-") as folder:
        local, remote = (Path(folder) / name for name in ("local.mkv", "remote.mkv"))
        broken = Path(folder) / "broken.mkv"
        broken.write_bytes(b"synthetic corrupt media")
        for media, color, resolution in ((local, "red", "160x90"), (remote, "blue", "240x136")):
            subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                            "-i", f"color={color}:size={resolution}:rate=10", "-t", "40",
                            "-c:v", "mpeg4", "-g", "10", str(media)], check=True)

        with ThreadingHTTPServer(("127.0.0.1", 0), Provider) as provider:
            provider.calls = Counter()
            provider.media = remote.read_bytes()
            provider.discovery_started = Event()
            provider.release_discovery = Event()
            provider.discovery_replied = Event()
            provider.release_discovery.set()
            Thread(target=provider.serve_forever, daemon=True).start()
            try:
                write_json(root / "providers.json", {"fixture": f"http://127.0.0.1:{provider.server_port}"})
                index = {
                    "movie:tmdb:900000001": {"local": [str(broken), str(local)],
                                              "query": "Pilot Movie"},
                    "episode:tmdb:900000002": {"title": "Return", "episode": "s01e02",
                                               "query": "Pilot Show", "local": [str(local)]},
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
                    print("Ordinary playback, local-first and explicit Stop passed", flush=True)

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
                    assert provider.calls["resolve"] == 2, provider.calls
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

                    index["episode:tmdb:900000002"]["local"] = []
                    write_json(root / "index.json", index)
                    provider.discovery_started.clear()
                    provider.release_discovery.clear()
                    provider.discovery_replied.clear()
                    kodi.call("Player.Open", item={"file": str(episode)})
                    assert provider.discovery_started.wait(3)
                    player_id = kodi.play(movie)
                    provider.release_discovery.set()
                    assert provider.discovery_replied.wait(3)
                    sleep(0.25)
                    assert provider.calls["resolve"] == 2, provider.calls
                    kodi.stop(player_id)
                    print("A late discovery response left the replacement playback alone")
            finally:
                provider.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("userdata", type=Path, help="userdata folder of an isolated Kodi profile")
    parser.add_argument("--port", type=int, default=19090)
    args = parser.parse_args()
    run(args.userdata.resolve(strict=True), args.port)
