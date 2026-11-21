"""Check whether Kodi's legacy OnStop end flag distinguishes a failed open from Stop."""

import argparse
import codecs
import json
import socket
import unittest
from pathlib import Path
from time import monotonic


class KodiConnection:
    def __init__(self, connection):
        self.connection = connection
        self.buffer = ""
        self.utf8_decoder = codecs.getincrementaldecoder("utf-8")()
        self.json_decoder = json.JSONDecoder()
        self.request_id = 0

    def request(self, method, **params):
        self.request_id += 1
        message = {"jsonrpc": "2.0", "id": self.request_id, "method": method, "params": params}
        self.connection.sendall(json.dumps(message).encode())
        return self.request_id

    def wait_for(self, method):
        deadline = monotonic() + 10
        while True:
            self.buffer = self.buffer.lstrip()
            try:
                message, end = self.json_decoder.raw_decode(self.buffer)
            except json.JSONDecodeError:
                remaining = deadline - monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"Kodi did not send {method}")
                self.connection.settimeout(remaining)
                packet = self.connection.recv(4096)
                if not packet:
                    raise ConnectionError(f"Kodi disconnected before {method}")
                self.buffer += self.utf8_decoder.decode(packet)
                continue

            self.buffer = self.buffer[end:]
            if "error" in message:
                raise RuntimeError(message["error"])
            if message.get("method") == method:
                return message["params"]["data"]


class TestStopSignal(unittest.TestCase):
    def test_stop_and_failed_open_are_distinguishable(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as connection:
            kodi = KodiConnection(connection)
            kodi.request("Player.Open", item={"file": str(self.playable)})
            started = kodi.wait_for("Player.OnAVStart")
            kodi.request("Player.Stop", playerid=started["player"]["playerid"])
            stopped = kodi.wait_for("Player.OnStop")
            self.assertIs(stopped["end"], False)

            kodi.request("Player.Open", item={"file": str(self.corrupt)})
            failed = kodi.wait_for("Player.OnStop")
            self.assertNotEqual(
                stopped["end"],
                failed["end"],
                "Viewer Stop and failed open both announced end=false",
            )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("playable", type=Path)
    parser.add_argument("corrupt", type=Path)
    parser.add_argument("--port", type=int, default=19090)
    args = parser.parse_args()
    TestStopSignal.playable = args.playable.resolve(strict=True)
    TestStopSignal.corrupt = args.corrupt.resolve(strict=True)
    TestStopSignal.port = args.port
    unittest.main(argv=[__file__], verbosity=2)
