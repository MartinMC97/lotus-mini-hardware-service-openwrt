#!/usr/bin/env python3
"""Host-only tests: no hardware nodes, HA connections or audio are touched."""
import json
import os
from pathlib import Path
import socket
import stat
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
DAEMON = os.environ.get("HW_TEST_DAEMON", str(ROOT / "build/lotus-hardwared"))


class Peer:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(2)
        self.socket.connect(str(path))
        self.file = self.socket.makefile("rb")
        self.id = 0
        self.events = []
        self.snapshot = self.read()

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError
        return json.loads(line)

    def request(self, method, params=None, version=1):
        self.id += 1
        self.socket.sendall(json.dumps(dict(version=version, id=self.id,
            method=method, params=params or {})).encode() + b"\n")
        while True:
            msg = self.read()
            if msg.get("id") == self.id:
                return msg
            self.events.append(msg)

    def close(self):
        self.file.close()
        self.socket.close()


class Integration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lotus-hw-test-")
        self.path = Path(self.temp.name) / "control.sock"
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([DAEMON,
            "--mock", "--debug", "--socket", str(self.path)], stderr=self.log)
        for _ in range(100):
            if self.path.exists():
                break
            if self.process.poll() is not None:
                self.log.seek(0)
                self.fail("service exited before ready: " + self.log.read().decode())
            time.sleep(.01)
        self.peers = []

    def peer(self):
        p = Peer(self.path)
        self.peers.append(p)
        return p

    def start_persistent_mock(self, directory, privacy=False):
        for p in self.peers:
            p.close()
        self.peers = []
        self.process.terminate()
        self.process.wait(timeout=3)
        self.process = subprocess.Popen([DAEMON, "--mock", "--socket", str(self.path),
            "--state-dir", str(directory)] + (["--privacy"] if privacy else []), stderr=self.log)
        for _ in range(100):
            if self.path.exists():
                return
            if self.process.poll() is not None:
                self.fail("persistent mock exited")
            time.sleep(.01)
        self.fail("persistent startup timeout")

    def wait_privacy(self, peer, muted):
        for _ in range(100):
            state = peer.request("state.get")["result"]
            if not state["privacy_persistence"]["pending"]:
                self.assertEqual(state["microphone_muted"], muted)
                return state
            time.sleep(.02)
        self.fail("privacy save timeout")

    def test_privacy_async_restore_and_fail_closed(self):
        directory = Path(self.temp.name) / "state"
        directory.mkdir(mode=0o700)
        self.start_persistent_mock(directory, privacy=True)
        p = self.peer()
        self.assertTrue(p.snapshot["data"]["microphone_muted"])
        self.assertFalse((directory / "privacy.json").exists())
        response = p.request("privacy.set", {"muted": False})["result"]
        self.assertTrue(response["muted"])
        self.assertTrue(response["persistence"]["pending"])
        self.wait_privacy(p, False)
        self.assertEqual(json.loads((directory / "privacy.json").read_text()),
            {"version": 1, "muted": False})
        self.start_persistent_mock(directory, privacy=True)
        p = self.peer()
        self.assertFalse(p.snapshot["data"]["microphone_muted"])
        p.request("privacy.set", {"muted": True})
        self.assertTrue(p.request("state.get")["result"]["microphone_muted"])
        self.wait_privacy(p, True)
        # Explicit unmute fails when safe-file checks reject the target.
        (directory / "privacy.json").unlink()
        (directory / "privacy.json").symlink_to("nonexistent")
        p.request("privacy.set", {"muted": False})
        state = self.wait_privacy(p, True)
        self.assertEqual(state["privacy_persistence"]["error"], "unsafe_target")
        self.start_persistent_mock(directory, privacy=True)
        self.assertTrue(self.peer().snapshot["data"]["microphone_muted"])

    def test_privacy_latest_request_wins_and_logs(self):
        directory = Path(self.temp.name) / "state"
        directory.mkdir(mode=0o700)
        self.start_persistent_mock(directory, privacy=True)
        p = self.peer()
        for _ in range(10):
            p.request("privacy.set", {"muted": False})
            p.request("privacy.set", {"muted": True})
        self.wait_privacy(p, True)
        self.assertTrue(json.loads((directory / "privacy.json").read_text())["muted"])
        self.start_persistent_mock(directory, privacy=True)
        self.assertTrue(self.peer().snapshot["data"]["microphone_muted"])

    def test_audio_registration_privacy_and_disconnect(self):
        p, other = self.peer(), self.peer()
        self.assertFalse(p.request("audio.acquire", {"capture": True, "playback": True})["ok"])
        self.assertTrue(p.request("lease.acquire", {"ttl_ms": 1000})["ok"])
        self.assertFalse(p.request("audio.acquire", {"capture": True, "playback": False})["ok"])
        self.assertTrue(p.request("audio.acquire", {"capture": False, "playback": True})["ok"])
        p.request("privacy.set", {"muted": False})
        self.assertTrue(p.request("audio.acquire", {"capture": True, "playback": True})["ok"])
        self.assertFalse(other.request("audio.acquire", {"capture": False, "playback": True})["ok"])
        p.request("privacy.set", {"muted": True})
        self.assertEqual(p.request("state.get")["result"]["audio"]["owner_connection"], 0)
        self.assertTrue(p.request("audio.acquire", {"capture": False, "playback": True})["ok"])
        p.close(); self.peers.remove(p)
        time.sleep(.1)
        self.assertEqual(other.request("state.get")["result"]["audio"]["owner_connection"], 0)

        self.assertTrue(other.request("lease.acquire", {"ttl_ms": 1000})["ok"])
        self.assertTrue(other.request("audio.acquire", {"capture": False, "playback": True})["ok"])
        time.sleep(1.1)
        self.assertEqual(other.request("state.get")["result"]["audio"]["owner_connection"], 0)

    def test_muted_led_owned_by_application_not_privacy_policy(self):
        directory = Path(self.temp.name) / "state"
        directory.mkdir(mode=0o700)
        self.start_persistent_mock(directory, privacy=True)
        p, other = self.peer(), self.peer()
        self.assertTrue(p.request("lease.acquire", {"ttl_ms": 15000})["ok"])
        effect = {"effect": "solid", "color": 0xff0000, "brightness_percent": 50}
        self.assertTrue(p.request("led.set", effect)["ok"])
        self.assertEqual(p.request("state.get")["result"]["led"], effect)
        self.assertFalse(other.request("led.set", effect)["ok"])
        self.assertFalse(p.request("audio.acquire", {"capture": True, "playback": True})["ok"])
        p.request("privacy.set", {"muted": False})
        self.wait_privacy(p, False)
        p.request("privacy.set", {"muted": True})
        self.wait_privacy(p, True)
        self.assertEqual(p.request("state.get")["result"]["led"], effect)
        timed = dict(effect, duration_ms=100)
        self.assertTrue(p.request("led.set", timed)["ok"])
        time.sleep(.2)
        self.assertEqual(p.request("state.get")["result"]["led"]["effect"], "off")
        self.assertTrue(p.request("led.set", effect)["ok"])
        p.close(); self.peers.remove(p)
        time.sleep(.1)
        self.assertEqual(other.request("state.get")["result"]["led"]["effect"], "off")
        self.assertTrue(other.request("lease.acquire", {"ttl_ms": 1000})["ok"])
        self.assertTrue(other.request("audio.acquire", {"capture": False, "playback": True})["ok"])
        time.sleep(1.1)
        self.assertEqual(other.request("state.get")["result"]["audio"]["owner_connection"], 0)

    def test_application_client_disconnect_and_timed_led(self):
        process = subprocess.Popen([str(ROOT / "build/application-client-test"), str(self.path)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(), "ready")
            p = self.peer()
            time.sleep(.15)
            self.assertEqual(p.request("state.get")["result"]["led"]["effect"], "off")
            self.process.terminate()
            self.process.wait(timeout=3)
            out, _ = process.communicate("\n", timeout=3)
            self.assertEqual(process.returncode, 0)
            self.assertIn("application client tests passed", out)
        finally:
            if process.poll() is None:
                process.kill(); process.wait(timeout=3)

    def tearDown(self):
        for p in self.peers:
            p.close()
        self.process.terminate()
        self.process.wait(timeout=3)
        self.log.seek(0)
        for line in self.log:
            record = json.loads(line)
            self.assertEqual(record["tag"], "lotus-hardwared")
            self.assertIn("instance", record)
        self.assertFalse(self.path.exists())
        self.log.close()
        self.temp.cleanup()

    def start_controls(self, root, confirm=True):
        self.process.terminate()
        self.process.wait(timeout=3)
        args = [DAEMON, "--controls", "--socket", str(self.path),
                "--sysfs-root", str(root), "--input-root", str(root / "input")]
        if confirm:
            args.append("--handoff-confirmed")
        self.process = subprocess.Popen(args, stderr=self.log)
        if confirm:
            for _ in range(100):
                if self.path.exists():
                    return
                if self.process.poll() is not None:
                    self.fail("controls daemon exited")
                time.sleep(.01)
            self.fail("controls startup timeout")

    def test_controls_handoff_required_and_missing_nodes(self):
        root = Path(self.temp.name) / "fixture"
        self.start_controls(root, confirm=False)
        self.assertEqual(self.process.wait(timeout=3), 2)
        self.assertFalse(self.path.exists())
        # Plain CLI usage diagnostic is not a JSON service record.
        self.log.seek(0)
        self.log.truncate()
        self.start_controls(root)
        p = self.peer()
        cap = p.request("capabilities.get")["result"]
        self.assertEqual(cap["backend"], "sysfs_controls")
        self.assertFalse(cap["led"]["rgb"])
        self.assertFalse(cap["buttons"]["available"])
        self.assertIsNone(p.request("state.get")["result"]["microphone_muted"])
        self.assertEqual(p.request("volume.set", {"percent": 20})["error"]["code"], "not_supported")
        self.assertEqual(p.request("privacy.set", {"muted": False})["error"]["code"], "not_supported")
        self.assertTrue(p.request("lease.acquire", {"ttl_ms": 1000})["ok"])
        self.assertEqual(p.request("led.set", {"effect": "off"})["error"]["code"], "device_error")
        # Missing channels can recover only through an explicit new command.
        for channel in ("redled", "greenled", "blueled"):
            node = root / "class/leds" / channel / "brightness"
            node.parent.mkdir(parents=True)
            node.write_text("0")
        self.assertTrue(p.request("led.set", {"effect": "solid", "color": 0xffffff,
            "brightness_percent": 100})["ok"])
        time.sleep(.12)
        self.assertTrue(p.request("capabilities.get")["result"]["led"]["rgb"])
        self.assertTrue(p.request("lease.release")["ok"])

    def test_controls_led_lease_switch_cancel_and_shutdown(self):
        root = Path(self.temp.name) / "fixture"
        nodes = []
        for channel in ("redled", "greenled", "blueled"):
            node = root / "class/leds" / channel / "brightness"
            node.parent.mkdir(parents=True)
            node.write_text("0")
            nodes.append(node)
        self.start_controls(root)
        a, b = self.peer(), self.peer()
        self.assertTrue(a.request("lease.acquire", {"ttl_ms": 1000})["ok"])
        self.assertEqual(b.request("lease.acquire", {"ttl_ms": 1000})["error"]["code"], "busy")
        params = {"effect": "solid", "color": 0xffffff, "brightness_percent": 100}
        self.assertTrue(a.request("led.set", params)["ok"])
        time.sleep(.06)
        self.assertEqual([p.read_text() for p in nodes], ["1"]*3)
        params.update(effect="gradient", color=0x0000ff, color_2=0x00ffff, period_ms=100)
        self.assertTrue(a.request("led.set", params)["ok"])
        self.assertTrue(a.request("led.set", {"effect": "off"})["ok"])
        time.sleep(.12)
        self.assertEqual([p.read_text() for p in nodes], ["0"]*3)
        self.assertTrue(a.request("led.set", dict(effect="solid", color=0xff0000,
            brightness_percent=100))["ok"])
        a.close()
        self.peers.remove(a)
        time.sleep(.12)
        self.assertEqual([p.read_text() for p in nodes], ["0"]*3)
        self.assertTrue(b.request("lease.acquire", {"ttl_ms": 1000})["ok"])
        self.assertTrue(b.request("led.set", dict(effect="solid", color=0xffffff,
            brightness_percent=100))["ok"])
        time.sleep(1.12)
        self.assertEqual(b.request("state.get")["result"]["owner_connection"], 0)
        self.assertEqual([p.read_text() for p in nodes], ["0"]*3)
        self.assertTrue(b.request("lease.acquire", {"ttl_ms": 1000})["ok"])
        self.assertTrue(b.request("led.set", dict(effect="solid", color=0xffffff,
            brightness_percent=100))["ok"])
        self.process.terminate()
        self.process.wait(timeout=3)
        self.assertEqual([p.read_text() for p in nodes], ["0"]*3)

    def test_snapshot_capabilities_permissions_cli(self):
        p = self.peer()
        self.assertEqual(stat.S_IMODE(os.stat(self.path).st_mode), 0o600)
        self.assertTrue(p.snapshot["data"]["microphone_muted"])
        self.assertFalse(p.snapshot["data"]["persistent"])
        self.assertEqual(p.request("capabilities.get")["result"]["backend"], "mock")
        cli = subprocess.run([str(ROOT / "build/lotus-hwctl"), "--socket",
            str(self.path), "state.get"], capture_output=True, text=True)
        self.assertEqual(cli.returncode, 0, cli.stderr)
        self.assertTrue(json.loads(cli.stdout)["ok"])

    def test_validation_and_redaction(self):
        p = self.peer()
        self.assertEqual(p.request("state.get", version=2)["error"]["code"], "unsupported_version")
        self.assertEqual(p.request("not-a-method")["error"]["code"], "not_supported")
        for value in (-1, 101, 2.5, "35", True):
            self.assertFalse(p.request("volume.set", {"percent": value})["ok"])
        self.assertFalse(p.request("privacy.set", {"muted": "no"})["ok"])
        for raw in (b"not json", b'{"version":1} trailing', b"[" * 30 + b"]" * 30,
                    b'{"version":1,"version":1,"id":1,"method":"state.get","params":{}}',
                    b'{"token":"SECRET_DO_NOT_LOG\xff"}'):
            p.socket.sendall(raw + b"\n")
            self.assertEqual(p.read()["error"]["code"], "invalid_request")
        self.log.seek(0)
        self.assertNotIn(b"SECRET_DO_NOT_LOG", self.log.read())
        self.assertTrue(p.request("state.get")["ok"])

    def test_fragmented_and_batched_frames(self):
        p = self.peer()
        req = b'{"version":1,"id":80,"method":"state.get","params":{}}\n'
        p.socket.sendall(req[:10])
        p.socket.sendall(req[10:] + req.replace(b"80", b"81"))
        self.assertEqual(p.read()["id"], 80)
        self.assertEqual(p.read()["id"], 81)

    def test_global_state_subscription(self):
        a, b = self.peer(), self.peer()
        self.assertTrue(b.request("events.subscribe")["ok"])
        a.request("volume.set", {"percent": 41})
        event = b.read()
        self.assertEqual(event["event"], "volume.changed")
        self.assertEqual(event["data"]["percent"], 41)
        a.request("privacy.set", {"muted": False})
        self.assertEqual(b.read()["event"], "privacy.changed")
        self.assertEqual(b.request("state.get")["result"]["volume_percent"], 41)

    def test_volume_adjust_validation_and_clamping(self):
        p = self.peer()
        self.assertEqual(p.request("volume.adjust", {"delta": 5})["result"]["percent"], 40)
        self.assertEqual(p.request("volume.adjust", {"delta": -100})["result"]["percent"], 0)
        self.assertEqual(p.request("volume.adjust", {"delta": 100})["result"]["percent"], 100)
        for value in (0, -101, 101, 1.5, True, "5"):
            self.assertFalse(p.request("volume.adjust", {"delta": value})["ok"])

    def test_volume_persistence_coalesce_restart_and_exit_flush(self):
        directory = Path(self.temp.name) / "state"
        directory.mkdir(mode=0o700)
        self.start_persistent_mock(directory)
        p = self.peer()
        target = directory / "volume.json"
        self.assertFalse(target.exists())
        self.assertEqual(p.snapshot["data"]["volume_percent"], 35)
        for percent in (40, 41, 42):
            response = p.request("volume.set", {"percent": percent})
            self.assertTrue(response["ok"])
            self.assertEqual(response["result"]["persistence"]["status"], "pending")
        self.assertFalse(target.exists())
        time.sleep(2.2)
        self.assertEqual(json.loads(target.read_text()), {"version": 1, "percent": 42})
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)
        self.assertFalse(list(directory.glob("volume.tmp.*")))
        self.start_persistent_mock(directory)
        p = self.peer()
        self.assertEqual(p.snapshot["data"]["volume_percent"], 42)
        stamp = target.stat().st_mtime_ns
        p.request("volume.set", {"percent": 42})
        self.assertEqual(target.stat().st_mtime_ns, stamp)
        p.request("volume.set", {"percent": 43})
        self.process.terminate()
        self.process.wait(timeout=3)
        self.assertEqual(json.loads(target.read_text())["percent"], 43)

    def test_volume_persistence_corrupt_file_requires_explicit_repair(self):
        directory = Path(self.temp.name) / "state"
        directory.mkdir(mode=0o700)
        target = directory / "volume.json"
        corrupt = '{"version":1,"percent":90,"percent":10}'
        target.write_text(corrupt)
        target.chmod(0o600)
        self.start_persistent_mock(directory)
        p = self.peer()
        self.assertEqual(p.snapshot["data"]["volume_percent"], 35)
        self.assertEqual(p.snapshot["data"]["volume_persistence"]["status"], "error")
        self.assertEqual(target.read_text(), corrupt)
        p.request("volume.set", {"percent": 37})
        time.sleep(2.2)
        self.assertEqual(json.loads(target.read_text())["percent"], 37)

    def test_volume_persistence_unsafe_target_and_directory_lock(self):
        directory = Path(self.temp.name) / "state"
        directory.mkdir(mode=0o700)
        victim = Path(self.temp.name) / "victim"
        victim.write_text("KEEP")
        (directory / "volume.json").symlink_to(victim)
        self.start_persistent_mock(directory)
        p = self.peer()
        response = p.request("volume.set", {"percent": 38})
        self.assertTrue(response["ok"])  # applied mock state, persistence is separate
        self.assertEqual(p.request("state.get")["result"]["volume_percent"], 38)
        time.sleep(2.2)
        state = p.request("state.get")["result"]["volume_persistence"]
        self.assertEqual(state["status"], "error")
        self.assertEqual(state["error"], "unsafe_target")
        self.assertEqual(victim.read_text(), "KEEP")
        other_path = Path(self.temp.name) / "other.sock"
        other = subprocess.run([DAEMON, "--mock", "--socket", str(other_path),
            "--state-dir", str(directory)], capture_output=True, timeout=3)
        self.assertNotEqual(other.returncode, 0)
        self.assertFalse(other_path.exists())
        self.assertTrue(p.request("state.get")["ok"])

    def test_volume_persistence_rejects_unsafe_directory_and_fifo(self):
        directory = Path(self.temp.name) / "state"
        directory.mkdir(mode=0o755)
        unsafe_path = Path(self.temp.name) / "unsafe.sock"
        result = subprocess.run([DAEMON, "--mock", "--socket", str(unsafe_path),
            "--state-dir", str(directory)], capture_output=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(unsafe_path.exists())
        directory.chmod(0o700)
        os.mkfifo(directory / "volume.json", 0o600)
        self.start_persistent_mock(directory)
        p = self.peer()
        self.assertEqual(p.snapshot["data"]["volume_persistence"]["status"], "error")
        self.assertTrue(p.request("state.get")["ok"])

    def test_quiet_tone_signal_and_warmup(self):
        result = subprocess.run([str(ROOT / "build/quiet-tone")], capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(len(result.stdout), 192000 * 4)
        samples = struct.unpack("<384000h", result.stdout)
        self.assertEqual(samples[0::2], samples[1::2])
        mono = samples[0::2]
        self.assertFalse(any(mono[:48000]))
        self.assertFalse(any(mono[144000:]))
        self.assertGreater(max(abs(v) for v in mono), 2500)
        self.assertLessEqual(max(abs(v) for v in mono), 2622)
        self.assertLess(max(abs(v) for v in mono[48000:48240]), 700)
        self.assertLess(max(abs(v) for v in mono[143760:144000]), 700)

    def test_bundled_lease_disconnect_and_no_takeover(self):
        a, b = self.peer(), self.peer()
        lease = a.request("lease.acquire", {"ttl_ms": 2000})
        self.assertEqual(lease["result"]["resources"], ["led", "action_buttons"])
        self.assertEqual(b.request("lease.acquire", {"ttl_ms": 2000})["error"]["code"], "busy")
        self.assertFalse(b.request("led.set", {"effect": "off"})["ok"])
        self.assertTrue(a.request("led.set", {"effect": "gradient", "color": 255,
            "color_2": 0x18bcf2, "brightness_percent": 30, "period_ms": 3000})["ok"])
        a.close()
        self.peers.remove(a)
        time.sleep(.15)
        state = b.request("state.get")["result"]
        self.assertEqual(state["owner_connection"], 0)
        self.assertEqual(state["led"]["effect"], "off")
        self.assertTrue(b.request("lease.acquire", {"ttl_ms": 1000})["ok"])

    def test_expiry_renew_and_led_validation(self):
        p = self.peer()
        self.assertFalse(p.request("lease.renew", {"ttl_ms": 1000})["ok"])
        p.request("lease.acquire", {"ttl_ms": 1000})
        self.assertFalse(p.request("led.set", {"effect": "rainbow"})["ok"])
        self.assertFalse(p.request("led.set", {"effect": "solid", "color": -1,
            "brightness_percent": 30})["ok"])
        time.sleep(1.15)
        self.assertEqual(p.request("state.get")["result"]["owner_connection"], 0)
        self.assertFalse(p.request("led.set", {"effect": "off"})["ok"])

    def test_oversized_disconnect(self):
        p = self.peer()
        p.socket.sendall(b"x" * 17000)
        self.assertEqual(p.socket.recv(1), b"")

    def test_existing_socket_not_unlinked(self):
        other = subprocess.run([DAEMON, "--mock",
            "--socket", str(self.path)], capture_output=True, timeout=3)
        self.assertNotEqual(other.returncode, 0)
        self.assertTrue(self.peer().request("state.get")["ok"])

    def test_log_rate_limit_and_multi_client_fairness(self):
        noisy, observer = self.peer(), self.peer()
        for i in range(180):
            self.assertTrue(noisy.request("volume.set", {"percent": i % 100})["ok"])
        before = time.monotonic()
        self.assertTrue(observer.request("state.get")["ok"])
        self.assertLess(time.monotonic() - before, .5)
        self.log.seek(0)
        self.assertLessEqual(len(self.log.readlines()), 100)
        time.sleep(1.05)
        observer.request("state.get")
        self.log.seek(0)
        self.assertIn(b"suppressed_logs", self.log.read())

    def test_unsafe_parent_rejected(self):
        directory = Path(self.temp.name) / "unsafe"
        directory.mkdir(mode=0o777)
        directory.chmod(0o777)
        other = subprocess.run([DAEMON, "--mock",
            "--socket", str(directory / "test.sock")], capture_output=True, timeout=3)
        self.assertNotEqual(other.returncode, 0)
        self.assertIn(b"unsafe_socket_directory", other.stderr)

    def test_cpp_client_and_persistent_cli_session(self):
        tested = subprocess.run([str(ROOT / "build/connection-test"), str(self.path)],
            capture_output=True, text=True, timeout=4)
        self.assertEqual(tested.returncode, 0, tested.stderr)
        commands = [dict(method="lease.acquire", params={"ttl_ms": 3000}),
                    dict(method="led.set", params={"effect": "off"}),
                    dict(method="state.get", params={})]
        cli = subprocess.run([str(ROOT / "build/lotus-hwctl"), "--socket",
            str(self.path), "session"], input="".join(json.dumps(c)+"\n" for c in commands),
            capture_output=True, text=True, timeout=4)
        self.assertEqual(cli.returncode, 0, cli.stderr)
        replies = [json.loads(line) for line in cli.stdout.splitlines()]
        self.assertEqual(len(replies), 3)
        self.assertTrue(all(r["ok"] for r in replies))
        self.assertGreater(replies[-1]["result"]["owner_connection"], 0)

    def test_cli_watch(self):
        watcher = subprocess.Popen([str(ROOT / "build/lotus-hwctl"), "--socket",
            str(self.path), "watch"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.assertTrue(json.loads(watcher.stdout.readline())["ok"])
            self.assertEqual(json.loads(watcher.stdout.readline())["event"], "snapshot")
            self.peer().request("volume.set", {"percent": 19})
            self.assertEqual(json.loads(watcher.stdout.readline())["event"], "volume.changed")
            watcher.terminate()
            self.assertEqual(watcher.wait(timeout=2), 0)
        finally:
            if watcher.poll() is None:
                watcher.kill()
                watcher.wait()
            watcher.stdout.close()
            watcher.stderr.close()

    def test_blocked_log_receiver_does_not_block_service(self):
        path = Path(self.temp.name) / "blocked-log.sock"
        process = subprocess.Popen([DAEMON, "--mock", "--debug", "--socket", str(path)],
            stderr=subprocess.PIPE)
        try:
            for _ in range(100):
                if path.exists():
                    break
                time.sleep(.01)
            client = Peer(path)
            try:
                for i in range(300):
                    self.assertTrue(client.request("volume.set", {"percent": i % 100})["ok"])
                start = time.monotonic()
                self.assertTrue(client.request("state.get")["ok"])
                self.assertLess(time.monotonic() - start, .5)
            finally:
                client.close()
            start = time.monotonic()
            process.terminate()
            process.wait(timeout=1.5)
            self.assertLess(time.monotonic() - start, 1)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stderr.close()

    def test_readonly_sensors_and_control_rejection(self):
        root = Path(self.temp.name) / "sysfs"
        battery = root / "class/power_supply/lotus-battery"
        battery.mkdir(parents=True)
        (battery / "capacity").write_text("75\n")
        (battery / "status").write_text("Charging\n")
        zone = root / "class/thermal/thermal_zone7"
        zone.mkdir(parents=True)
        (zone / "type").write_text("soc-thermal\n")
        (zone / "temp").write_text("48181\n")
        path = Path(self.temp.name) / "sensors.sock"
        log = tempfile.TemporaryFile()
        process = subprocess.Popen([DAEMON,"--read-only","--sysfs-root",str(root),
            "--socket",str(path)], stderr=log)
        client = None
        try:
            for _ in range(100):
                if path.exists():
                    break
                time.sleep(.01)
            client = Peer(path)
            state = client.request("state.get")["result"]
            self.assertEqual(state["backend"], "sysfs_read_only")
            self.assertIsNone(state["volume_percent"])
            self.assertIsNone(state["microphone_muted"])
            self.assertEqual(state["battery"]["percent"],75)
            self.assertTrue(state["battery"]["external_power_connected"])
            self.assertEqual(state["temperature"]["readings"][0]["millidegrees_celsius"],48181)
            for method,params in (("volume.set",{"percent":90}), ("privacy.set",{"muted":False}),
                                  ("lease.acquire",{"ttl_ms":1000}), ("led.set",{"effect":"off"})):
                self.assertEqual(client.request(method,params)["error"]["code"],"not_supported")
            client.request("events.subscribe")
            (battery / "capacity").write_text("170\n")
            (battery / "status").write_text("Unknown\n")
            (zone / "temp").write_text("bad\n")
            time.sleep(1.2)
            state = client.request("state.get")["result"]
            self.assertFalse(state["battery"]["available"])
            self.assertIsNone(state["battery"]["percent"])
            self.assertIsNone(state["battery"]["external_power_connected"])
            self.assertFalse(state["temperature"]["available"])
            self.assertIn("battery.changed",[e["event"] for e in client.events])
            self.assertIn("temperature.changed",[e["event"] for e in client.events])
            (battery / "capacity").write_text("50\n")
            (zone / "temp").write_text("50000\n")
            duplicate = root / "class/thermal/thermal_zone11"
            duplicate.mkdir()
            (duplicate / "type").write_text("soc-thermal\n")
            (duplicate / "temp").write_text("51000\n")
            time.sleep(1.2)
            state = client.request("state.get")["result"]
            self.assertTrue(state["battery"]["available"])
            self.assertEqual(state["temperature"]["readings"][0]["error"],"ambiguous_type")
            self.assertEqual((battery / "capacity").read_text(),"50\n")
            (battery / "capacity").unlink()
            os.mkfifo(battery / "capacity")
            time.sleep(1.2)
            self.assertFalse(client.request("state.get")["result"]["battery"]["available"])
        finally:
            if client:
                client.close()
            process.terminate()
            process.wait(timeout=2)
            log.close()

    def test_sensor_root_absent_degrades(self):
        path = Path(self.temp.name) / "missing.sock"
        process = subprocess.Popen([DAEMON,"--read-only","--sysfs-root",
            str(Path(self.temp.name)/"absent"),"--socket",str(path)],stderr=subprocess.DEVNULL)
        client=None
        try:
            for _ in range(100):
                if path.exists():
                    break
                time.sleep(.01)
            client=Peer(path)
            result=client.request("state.get")["result"]
            self.assertFalse(result["battery"]["available"])
            self.assertFalse(result["temperature"]["available"])
        finally:
            if client:
                client.close()
            process.terminate()
            process.wait(timeout=2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
