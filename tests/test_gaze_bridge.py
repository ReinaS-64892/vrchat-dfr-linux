#!/usr/bin/env python3
"""Exercise the actual publisher against a fake OpenXR runtime."""
import mmap
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())


def packet(path):
    try:
        with open(path, "rb") as file:
            if os.fstat(file.fileno()).st_size != 64:
                return None
            with mmap.mmap(file.fileno(), 64, access=mmap.ACCESS_READ) as mem:
                words = struct.unpack("<16I", mem[:])
                seq = struct.unpack("<I", mem[12:16])[0]
                if seq != words[3] or seq & 1 or words[:3] != (0x325A4147, 1, 64):
                    return None
                return words
    except FileNotFoundError:
        return None


def wait_for(path, predicate, timeout=2):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = packet(path)
        if value and predicate(value):
            return value
        time.sleep(.005)
    raise AssertionError(f"Timed out waiting for packet: {path}")


def launch(path, mode="valid", *args):
    env = dict(os.environ, GAZE_TEST_MODE=mode)
    return subprocess.Popen([binary, "--file", str(path), *args], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def stop(process):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
    try:
        return process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise


with tempfile.TemporaryDirectory(prefix="gaze-bridge-test-") as directory:
    base = Path(directory)
    path = base / "gaze"
    process = launch(path, "valid", "--distance", "1")
    try:
        words = wait_for(path, lambda p: p[4] == 1)
        uv = struct.unpack("<4f", struct.pack("<4I", *words[5:9]))
        assert abs(uv[0]-.516) < .0001 and abs(uv[1]-.484) < .0001, uv
        assert uv[2:] == (.5, .5), uv
        assert path.stat().st_mode & 0o777 == 0o600
        inode = path.stat().st_ino
        # A competing writer must fail before changing the live mapping.
        competitor = launch(path)
        _, error = competitor.communicate(timeout=3)
        assert competitor.returncode == 1 and "lock" in error
        wait_for(path, lambda p: p[4] == 1 and p[3] != words[3])
        # Retain an mmap exactly as DXVK does, including across writer restarts.
        with path.open("rb") as file, mmap.mmap(file.fileno(), 64, access=mmap.ACCESS_READ) as memory:
            stop(process)
            assert packet(path)[4] == 0
            assert struct.unpack("<I", memory[16:20])[0] == 0
            process = launch(path)
            wait_for(path, lambda p: p[4] == 1)
            assert path.stat().st_ino == inode
            assert struct.unpack("<I", memory[16:20])[0] == 1
            # SIGKILL leaves a valid packet, but its sequence stops advancing.
            process.kill()
            process.communicate(timeout=3)
            frozen = packet(path)
            time.sleep(.15)
            assert packet(path) == frozen
    finally:
        stop(process)

    for mode in ("inactive", "untracked", "unfocused", "stale", "bad-view", "bad-count", "no-eye", "missing-extension"):
        result = launch(base/mode, mode, "--seconds", ".15")
        _, error = result.communicate(timeout=3)
        assert result.returncode == 1, (mode, error)
        assert packet(base/mode)[4] == 0, mode
        assert "Gaze valid" not in error, (mode, error)

    result = launch(base/"unknown-time", "unknown-time", "--seconds", ".15")
    _, error = result.communicate(timeout=3)
    assert result.returncode == 0 and "Gaze valid" in error

    # Runtime unavailable at startup, then usable on retry.
    result = launch(base/"retry", "retry", "--seconds", "2.3")
    _, error = result.communicate(timeout=5)
    assert result.returncode == 0 and "retrying" in error and "Gaze valid" in error, error

    # Session loss invalidates immediately, then reconnects without replacing inode.
    path = base/"disconnect"
    result = launch(path, "disconnect")
    try:
        wait_for(path, lambda p: p[4] == 1)
        inode = path.stat().st_ino
        wait_for(path, lambda p: p[4] == 0)
        wait_for(path, lambda p: p[4] == 1, timeout=4)
        assert path.stat().st_ino == inode
    finally:
        stop(result)
    assert packet(path)[4] == 0

    target = base/"target"
    target.write_bytes(b"untouched")
    link = base/"symlink"
    link.symlink_to(target)
    result = launch(link)
    result.communicate(timeout=3)
    assert result.returncode == 1 and target.read_bytes() == b"untouched"
    result = launch(target)
    result.communicate(timeout=3)
    assert result.returncode == 1 and target.read_bytes() == b"untouched"

    for args in (("--hz", "0"), ("--hz", "nan"), ("--seconds", "0"), ("--distance", "-1"), ("--wrong", "1")):
        result = subprocess.run([binary, *args], capture_output=True, timeout=3)
        assert result.returncode == 2, args

print("Gaze bridge integration checks passed")
