#!/usr/bin/env python3
"""Optional emulated Palm NetLib test; requires a private TX session and SDK probe."""

import argparse
import array
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import uuid
import zipfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--core", type=Path, default=Path("out/frond"))
p.add_argument("--session", type=Path, required=True)
p.add_argument(
    "--probe", type=Path, default=Path("build/host-network-probe/FrondHostProbe.prc")
)
p.add_argument("--output", type=Path, required=True)
p.add_argument(
    "--audio",
    action="store_true",
    help="Capture the app ARM tone with the PulseAudio SDL backend",
)
p.add_argument(
    "--require-gapless",
    action="store_true",
    help="Fail when captured audio contains a gap of 10 ms or longer",
)
p.add_argument(
    "--wait-control",
    action="store_true",
    help="Use an immediate HTTP reply then a four-second guest task delay as the PCM control",
)
p.add_argument(
    "--public",
    action="store_true",
    help="Also resolve example.com and fetch its HTTP page",
)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
root = a.output.resolve()
core = a.core.resolve()
source = a.session.resolve()
original = hashlib.sha256(source.read_bytes()).hexdigest()
(root / "identity.json").write_text(
    json.dumps(
        {
            "sha256": {
                str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in (core, source, a.probe.resolve())
            },
            "loadavg_start": os.getloadavg(),
            "wait_control": a.wait_control,
            "measurement": "emulator; excludes physical device and output hardware",
        },
        indent=2,
    )
    + "\n"
)
request_seen = threading.Event()
release_response = threading.Event()
checkpoint_seen = threading.Event()


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(20)
        data = b""
        try:
            while b"\n" not in data and len(data) < 4096:
                part = self.request.recv(4096)
                if not part:
                    return
                data += part
            if data.startswith(b"GET "):
                request_seen.set()
                if b"GET /slow " in data:
                    if not release_response.wait(15):
                        return
                body = b"frond-host-ok\n"
                self.request.sendall(
                    b"HTTP/1.0 200 OK\r\nContent-Length: "
                    + str(len(body)).encode()
                    + b"\r\n\r\n"
                    + body
                )
            else:
                self.request.sendall(data)
                checkpoint_seen.set()
                while self.request.recv(4096):
                    pass
        except (OSError, TimeoutError):
            pass


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True


server = Server(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.bind(("127.0.0.1", 0))
udp.settimeout(0.2)
udp_stop = threading.Event()


def serve_udp():
    while not udp_stop.is_set():
        try:
            data, peer = udp.recvfrom(4096)
            if data == b"ping":
                udp.sendto(b"pong", peer)
        except socket.timeout:
            pass
        except OSError:
            return


threading.Thread(target=serve_udp, daemon=True).start()


def config(host, port, udp_port, path, label):
    text = f"{host}\n{port}\n{udp_port}\n{path}\n".encode() + b"\0"
    data = bytearray(88)
    data[:15] = b"FrondHostConfig"
    data[60:64] = b"DATA"
    data[64:68] = b"Fhcf"
    struct.pack_into(">H", data, 76, 1)
    struct.pack_into(">I", data, 78, 88)
    data[85] = 1
    out = root / (label + ".pdb")
    out.write_bytes(data + text)
    return out


def records(path):
    with zipfile.ZipFile(path) as z:
        raw = z.read(next(n for n in z.namelist() if "FrondHostLog" in n))
    count = struct.unpack_from(">H", raw, 76)[0]
    offsets = [struct.unpack_from(">I", raw, 78 + i * 8)[0] for i in range(count)] + [
        len(raw)
    ]
    result = [raw[x:y].rstrip(b"\0").decode() for x, y in zip(offsets, offsets[1:])]
    print("\n".join(result), flush=True)
    return result


def retry(request, command, argument):
    for _ in range(12):
        ok, message = request(command, argument, ok=None)
        if ok:
            return
        time.sleep(3)
    raise AssertionError(message)


def pulse(kind):
    return json.loads(
        subprocess.check_output(["pactl", "-f", "json", "list", kind], text=True)
    )


@contextmanager
def emulator(session, label, audio=False):
    with (
        tempfile.TemporaryDirectory(prefix="frond-host-") as directory,
        (root / (label + ".log")).open("wb") as log,
    ):
        listener = socket.socket(socket.AF_UNIX)
        address = str(Path(directory) / "control")
        listener.bind(address)
        listener.listen(1)
        listener.settimeout(45)
        token = uuid.uuid4().hex
        command = (["heavy", "emulator"] if shutil.which("heavy") else []) + [
            str(core),
            str(session),
            "--no-sound",
            "--desktop-socket",
            address,
        ]
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            env=dict(
                os.environ,
                SDL_VIDEODRIVER="dummy",
                SDL_AUDIODRIVER="pulseaudio" if audio else "dummy",
                PULSE_PROP="frond.host_check=" + token,
            ),
        )
        connection = stream = recorder = raw = None
        try:
            connection, _ = listener.accept()
            connection.settimeout(120)
            stream = connection.makefile("rb")
            assert stream.readline() == b"0\tevent\tready\t\n"
            serial = 0

            def request(command, argument="", ok=True):
                nonlocal serial
                serial += 1
                connection.sendall(
                    f"{serial}\t{command}\t{str(argument).encode().hex()}\n".encode()
                )
                fields = stream.readline().rstrip(b"\n").split(b"\t")
                assert len(fields) == 4 and fields[0] == str(serial).encode(), fields
                message = bytes.fromhex(fields[3].decode()).decode()
                print(label, command, fields[1].decode(), message, flush=True)
                if ok is not None:
                    assert fields[1] == (b"ok" if ok else b"error"), fields
                return (fields[1] == b"ok", message) if ok is None else message

            if audio:
                request("audio-on")
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    item = next(
                        (
                            s
                            for s in pulse("sink-inputs")
                            if s.get("properties", {}).get("frond.host_check") == token
                        ),
                        None,
                    )
                    if item:
                        break
                    time.sleep(0.1)
                assert item, "own PulseAudio stream not found"
                assert (
                    os.getpgid(int(item["properties"]["application.process.id"]))
                    == process.pid
                )
                sink = next(s for s in pulse("sinks") if s["index"] == item["sink"])
                monitor = sink.get("monitor_source_name", sink.get("monitor_source"))
                if isinstance(monitor, int):
                    monitor = next(
                        s["name"] for s in pulse("sources") if s["index"] == monitor
                    )
                raw = (root / "wait-tone.raw").open("wb")
                recorder = subprocess.Popen(
                    [
                        "parec",
                        "--device=" + monitor,
                        "--monitor-stream=" + str(item["index"]),
                        "--format=s16le",
                        "--rate=44100",
                        "--channels=2",
                        "--raw",
                    ],
                    stdout=raw,
                    stderr=subprocess.DEVNULL,
                )

            def stop_capture():
                nonlocal recorder, raw
                if recorder:
                    recorder.terminate()
                    recorder.wait(timeout=10)
                    recorder = None
                if raw:
                    raw.close()
                    raw = None

            request.stop_capture = stop_capture
            time.sleep(2)
            yield request
            if recorder:
                recorder.terminate()
                recorder.wait(timeout=10)
                recorder = None
            if raw:
                raw.close()
                raw = None
            request("quit")
            assert process.wait(timeout=20) == 0
        finally:
            if recorder:
                recorder.terminate()
                recorder.wait(timeout=10)
            if raw:
                raw.close()
            if stream:
                stream.close()
            if connection:
                connection.close()
            listener.close()
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


try:
    settings = config(
        "localhost",
        server.server_address[1],
        udp.getsockname()[1],
        "/wait-control" if a.wait_control else "/slow",
        "local-config",
    )
    with emulator(source, "local", a.audio) as request:
        request("network-mode", "internet")
        retry(request, "install", settings)
        retry(request, "install", a.probe.resolve())
        retry(request, "launch-app", "Frond host probe")
        assert request_seen.wait(10), "HTTP request never reached the local server"
        started = time.monotonic()
        assert request("network-state") == "internet"
        elapsed = time.monotonic() - started
        assert elapsed < 2, "host UI control blocked during guest Receive"
        (root / "wait-responsiveness.json").write_text(
            json.dumps(
                {
                    "rpc_seconds_during_blocked_receive": elapsed,
                    "measurement": "emulator host-loop response, not physical device latency",
                },
                indent=2,
            )
            + "\n"
        )
        time.sleep(4)
        release_response.set()
        assert checkpoint_seen.wait(30), "probe did not finish network/tone checks"
        time.sleep(2)
        request.stop_capture()  # Exclude export pauses and later, separate tone runs.
        shot = root / "stage.replay"
        shot.write_text("100 snapshot " + str(root / "stage.png") + "\n")
        request("input-replay", shot)
        time.sleep(1)
        retry(request, "export", root / "pending.zip")
        rows = records(root / "pending.zip")
        for expected in [
            "http-header err=0000 value=0",
            "http-body err=0000 value=0",
            "udp-select err=0000 value=1",
            "udp-peek err=0000 value=4",
            "udp-receive err=0000 value=4",
            "ready err=0000 value=0",
        ]:
            assert expected in rows, expected
        assert any(r.startswith("udp-match err=0000") for r in rows)
        assert any(r.startswith("udp-wouldblock err=122F") for r in rows)
        assert any(r.startswith("udp-timeout err=1212") for r in rows)
        assert any(
            r.startswith("tone-calls err=0000") and int(r.split("value=")[1]) > 5
            for r in rows
        ), "ARM sound callbacks did not progress during HTTP wait"
        read = root / "read.replay"
        read.write_text("500 pen-down 100 140\n650 pen-up\n")
        request("input-replay", read)
        time.sleep(2)
        retry(request, "export", root / "after-export.zip")
        assert "checkpoint-match err=0000 value=0" in records(
            root / "after-export.zip"
        ), "export lost the live TCP connection"
        checkpoint_seen.clear()
        retry(request, "launch-app", "Frond host probe")
        assert checkpoint_seen.wait(30), "second probe run did not finish"
        time.sleep(2)
        request("save", root / "connected.frond")
    with emulator(root / "connected.frond", "restored") as request:
        assert request("network-state") == "internet"
        request("input-replay", read)
        time.sleep(2)
        retry(request, "export", root / "restored.zip")
        rows = records(root / "restored.zip")
        assert any(r.startswith("checkpoint-receive err=1214") for r in rows), (
            "restored host connections must be closed"
        )
        checkpoint_seen.clear()
        retry(request, "launch-app", "Frond host probe")
        assert checkpoint_seen.wait(30), "reconnected probe run did not finish"
        time.sleep(2)
        request("input-replay", read)
        time.sleep(2)
        retry(request, "export", root / "reconnected.zip")
        assert (
            records(root / "reconnected.zip")[-1] == "checkpoint-match err=0000 value=0"
        )
    if a.audio:
        samples = array.array("h")
        samples.frombytes((root / "wait-tone.raw").read_bytes())
        signal = [i for i, x in enumerate(samples) if abs(x) > 50]
        assert signal, "captured tone is silent"
        span = samples[signal[0] : signal[-1] + 1]
        run = longest = 0
        for x in span:
            run = run + 1 if x == 0 else 0
            longest = max(run, longest)
        result = {
            "seconds_between_first_last_signal": len(span) / 88200,
            "peak": max(map(abs, span)),
            "longest_exact_zero_ms": longest / 88.2,
            "measurement": "emulator PCM, excludes physical output",
            "wait_control": a.wait_control,
            "gapless_below_10_ms": longest / 88.2 < 10,
        }
        (root / "tone-check.json").write_text(json.dumps(result, indent=2) + "\n")
        print(result, flush=True)
        assert (
            result["seconds_between_first_last_signal"] >= 3 and result["peak"] < 32767
        )
        if a.require_gapless:
            assert result["longest_exact_zero_ms"] < 10, (
                "captured audio contains a gap of at least 10 ms"
            )
    if a.public:
        with emulator(source, "public") as request:
            request("network-mode", "internet")
            retry(request, "install", a.probe.resolve())
            retry(request, "launch-app", "Frond host probe")
            time.sleep(4)
            retry(request, "export", root / "public.zip")
            rows = records(root / "public.zip")
            assert (
                "http-header err=0000 value=0" in rows
                and "http-body err=0000 value=0" in rows
            )
    assert hashlib.sha256(source.read_bytes()).hexdigest() == original
    print(
        "PASS: Palm host DNS/TCP/UDP, cooperative waits, live export checkpoint, closed connections on restore and reconnect; input unchanged",
        flush=True,
    )
finally:
    release_response.set()
    udp_stop.set()
    udp.close()
    server.shutdown()
    server.server_close()
